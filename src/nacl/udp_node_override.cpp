// Overrides nacl_io's UdpNode (from libnacl_io.a) so that:
// 1. Transient UDP SendTo/RecvFrom errors (e.g., 255.255.255.255 ENETUNREACH or ICMP
//    port unreachable from an offline host) do NOT permanently mark the UDP socket
//    as SSF_CLOSED / clear SSF_CAN_SEND | SSF_CAN_RECV.
// 2. The per-socket RX/TX FIFO size is 2 MB instead of 512 KB, preventing drops on
//    large H.264 keyframe bursts.

#ifndef NDEBUG
#define NDEBUG 1
#endif

#include "nacl_io/socket/udp_node.h"

#include <errno.h>
#include <string.h>

#include <algorithm>

#include "nacl_io/log.h"
#include "nacl_io/pepper_interface.h"
#include "nacl_io/socket/packet.h"
#include "nacl_io/socket/udp_event_emitter.h"
#include "nacl_io/stream/stream_fs.h"
#include "ppapi/c/pp_errors.h"

namespace {
const size_t kMaxPacketSize = 65536;
const size_t kDefaultFifoSize = 4 * 1024 * 1024;
const int32_t kDefaultRecvBufferSize = 4 * 1024 * 1024;
const int32_t kDefaultSendBufferSize = 1024 * 1024;
}  // namespace

namespace nacl_io {

UDPInterfaceWrapper12::UDPInterfaceWrapper12(UDPSocketInterface* udp_interface)
    : udp_interface_(udp_interface) {}

UDPInterfaceWrapper10::UDPInterfaceWrapper10(
    UDPSocketInterface10* udp_interface,
    UDPSocketExtensionSamsungInterface* udp_ext_interface)
    : udp_interface_(udp_interface), udp_ext_interface(udp_ext_interface) {}

int32_t UDPInterfaceWrapper10::SetOption(PP_Resource udp_socket,
                                         PP_UDPSocket_Option name,
                                         struct PP_Var value,
                                         struct PP_CompletionCallback callback) {
    PP_UDPSocket_Option option = name;
    switch (option) {
        case PP_UDPSOCKET_OPTION_MULTICAST_LOOP:
            option = PP_UDPSOCKET_EXT_OPTION_MULTICAST_LOOP;
            break;
        case PP_UDPSOCKET_OPTION_MULTICAST_TTL:
            option = PP_UDPSOCKET_EXT_OPTION_MULTICAST_TTL;
            break;
        default:
            break;
    }
    return udp_interface_->SetOption(udp_socket, option, value, callback);
}

int32_t UDPInterfaceWrapper10::JoinGroup(PP_Resource udp_socket,
                                         PP_Resource group,
                                         struct PP_CompletionCallback callback) {
    (void) callback;
    PP_MulticastMembership membership;
    PP_Resource multicast_addr = udp_interface_->GetBoundAddress(udp_socket);
    SetMulticastMembership(&membership, group, multicast_addr);
    return udp_ext_interface->JoinMulticast(udp_socket, &membership, PP_BlockUntilComplete());
}

int32_t UDPInterfaceWrapper10::LeaveGroup(PP_Resource udp_socket,
                                          PP_Resource group,
                                          struct PP_CompletionCallback callback) {
    (void) callback;
    PP_MulticastMembership membership;
    PP_Resource multicast_addr = udp_interface_->GetBoundAddress(udp_socket);
    SetMulticastMembership(&membership, group, multicast_addr);
    return udp_ext_interface->LeaveMulticast(udp_socket, &membership, PP_BlockUntilComplete());
}

void UDPInterfaceWrapper10::SetMulticastMembership(PP_MulticastMembership* membership,
                                                   PP_Resource interface_addr,
                                                   PP_Resource multicast_addr) {
    membership->interface_addr = interface_addr;
    membership->multicast_addr = multicast_addr;
    membership->source_addr = 0;
}

class UdpWork : public StreamFs::Work {
public:
    explicit UdpWork(const ScopedUdpEventEmitter& emitter)
        : StreamFs::Work(emitter->stream()->stream()), emitter_(emitter), packet_(NULL) {}

    ~UdpWork() override { delete packet_; }

protected:
    ScopedUdpEventEmitter emitter_;
    Packet* packet_;
};

class UdpSendWork : public UdpWork {
public:
    explicit UdpSendWork(const ScopedUdpEventEmitter& emitter, const ScopedSocketNode& node)
        : UdpWork(emitter), node_(node) {}

    bool Start(int32_t val) override {
        (void) val;
        AUTO_LOCK(emitter_->GetLock());

        if (!node_->TestStreamFlags(SSF_CAN_SEND)) return false;
        if (node_->TestStreamFlags(SSF_SENDING)) return false;

        if (NULL == packet_) {
            packet_ = emitter_->ReadTXPacket_Locked();
            if (NULL == packet_) return false;
        }

        UdpNode* udp_node = static_cast<UdpNode*>(node_.get());
        int err = udp_node->GetUDPInterfaceWrapper()->SendTo(
            node_->socket_resource(), packet_->buffer(), packet_->len(), packet_->addr(),
            filesystem()->GetRunCompletion(this));
        if (err != PP_OK_COMPLETIONPENDING) {
            if (err == PP_ERROR_ABORTED || err == PP_ERROR_BADRESOURCE) {
                node_->SetError_Locked(err);
                return false;
            }
            // Transient synchronous send error: drop this packet and try the next one.
            node_->QueueOutput();
            return false;
        }

        node_->SetStreamFlags(SSF_SENDING);
        return true;
    }

    void Run(int32_t length_error) override {
        AUTO_LOCK(emitter_->GetLock());

        if (length_error < 0) {
            if (length_error == PP_ERROR_INPROGRESS) {
                node_->ClearStreamFlags(SSF_SENDING);
                node_->stream()->EnqueueWork(this);
                return;
            }
            if (length_error == PP_ERROR_ABORTED || length_error == PP_ERROR_BADRESOURCE) {
                node_->SetError_Locked(length_error);
                return;
            }
            // Non-fatal UDP send error (e.g., unreachable broadcast/host): keep socket alive.
            node_->ClearStreamFlags(SSF_SENDING);
            node_->QueueOutput();
            return;
        }

        node_->ClearStreamFlags(SSF_SENDING);
        node_->QueueOutput();
    }

private:
    ScopedSocketNode node_;
};

class UdpRecvWork : public UdpWork {
public:
    explicit UdpRecvWork(const ScopedUdpEventEmitter& emitter) : UdpWork(emitter) {}

    bool Start(int32_t val) override {
        (void) val;
        AUTO_LOCK(emitter_->GetLock());
        UdpNode* stream = static_cast<UdpNode*>(emitter_->stream());

        if (NULL == stream || !stream->TestStreamFlags(SSF_CAN_RECV)) return false;
        if (stream->TestStreamFlags(SSF_RECVING)) return false;

        stream->SetStreamFlags(SSF_RECVING);
        int err = stream->GetUDPInterfaceWrapper()->RecvFrom(
            stream->socket_resource(), data_, kMaxPacketSize, &addr_,
            filesystem()->GetRunCompletion(this));
        if (err != PP_OK_COMPLETIONPENDING) {
            stream->ClearStreamFlags(SSF_RECVING);
            if (err == PP_ERROR_ABORTED || err == PP_ERROR_BADRESOURCE) {
                stream->SetError_Locked(err);
            }
            return false;
        }

        return true;
    }

    void Run(int32_t length_error) override {
        AUTO_LOCK(emitter_->GetLock());
        UdpNode* stream = static_cast<UdpNode*>(emitter_->stream());
        if (NULL == stream) return;

        if (length_error > 0) {
            Packet* packet = new Packet(filesystem()->ppapi());
            packet->Copy(data_, length_error, addr_);
            filesystem()->ppapi()->ReleaseResource(addr_);
            emitter_->WriteRXPacket_Locked(packet);
            stream->ClearStreamFlags(SSF_RECVING);
            stream->QueueInput();
        } else {
            stream->ClearStreamFlags(SSF_RECVING);
            if (length_error == PP_ERROR_ABORTED || length_error == PP_ERROR_BADRESOURCE) {
                stream->SetError_Locked(length_error);
            } else if (stream->TestStreamFlags(SSF_CAN_RECV)) {
                stream->QueueInput();
            }
        }
    }

private:
    char data_[kMaxPacketSize];
    PP_Resource addr_;
};

UdpNode::UdpNode(Filesystem* filesystem)
    : SocketNode(SOCK_DGRAM, filesystem),
      emitter_(new UdpEventEmitter(kDefaultFifoSize, kDefaultFifoSize)),
      udp_interface_wrapper_(NULL) {
    if (UDPInterface()) {
        udp_interface_wrapper_ = new UDPInterfaceWrapper12(UDPInterface());
    } else if (UDPInterface10() && UDPExtensionSamsungInterface()) {
        udp_interface_wrapper_ =
            new UDPInterfaceWrapper10(UDPInterface10(), UDPExtensionSamsungInterface());
    }

    emitter_->AttachStream(this);
}

void UdpNode::Destroy() {
    emitter_->DetachStream();
    SocketNode::Destroy();
}

UdpEventEmitter* UdpNode::GetEventEmitter() { return emitter_.get(); }

Error UdpNode::Init(int open_flags) {
    Error err = SocketNode::Init(open_flags);
    if (err != 0) return err;

    if (udp_interface_wrapper_ == NULL) {
        LOG_ERROR("Got NULL interfaces: UDP 1.2 and UDP 1.0");
        return EACCES;
    }

    socket_resource_ = udp_interface_wrapper_->Create(filesystem_->ppapi()->GetInstance());
    if (0 == socket_resource_) {
        LOG_ERROR("Unable to create UDP resource.");
        return EACCES;
    }

    udp_interface_wrapper_->SetOption(
        socket_resource_, PP_UDPSOCKET_OPTION_RECV_BUFFER_SIZE,
        PP_MakeInt32(kDefaultRecvBufferSize), PP_BlockUntilComplete());
    udp_interface_wrapper_->SetOption(
        socket_resource_, PP_UDPSOCKET_OPTION_SEND_BUFFER_SIZE,
        PP_MakeInt32(kDefaultSendBufferSize), PP_BlockUntilComplete());

    return 0;
}

void UdpNode::QueueInput() {
    UdpRecvWork* work = new UdpRecvWork(emitter_);
    stream()->EnqueueWork(work);
}

void UdpNode::QueueOutput() {
    if (!TestStreamFlags(SSF_CAN_SEND)) return;
    if (TestStreamFlags(SSF_SENDING)) return;

    UdpSendWork* work = new UdpSendWork(emitter_, ScopedSocketNode(this));
    stream()->EnqueueWork(work);
}

Error UdpNode::Bind(const struct sockaddr* addr, socklen_t len) {
    if (0 == socket_resource_) return EBADF;
    if (IsBound()) return EINVAL;

    PP_Resource out_addr = SockAddrToResource(addr, len);
    if (0 == out_addr) return EINVAL;

    int err = udp_interface_wrapper_->Bind(socket_resource_, out_addr, PP_BlockUntilComplete());
    filesystem_->ppapi()->ReleaseResource(out_addr);
    if (err != 0) return PPERROR_TO_ERRNO(err);

    out_addr = udp_interface_wrapper_->GetBoundAddress(socket_resource_);
    if (out_addr == 0) return EINVAL;

    SetStreamFlags(SSF_CAN_SEND | SSF_CAN_RECV);
    QueueInput();

    local_addr_ = out_addr;
    return 0;
}

Error UdpNode::Connect(const HandleAttr& attr, const struct sockaddr* addr, socklen_t len) {
    (void) attr;
    if (0 == socket_resource_) return EBADF;

    if (remote_addr_ != 0) {
        filesystem_->ppapi()->ReleaseResource(remote_addr_);
        remote_addr_ = 0;
    }

    remote_addr_ = SockAddrToResource(addr, len);
    if (0 == remote_addr_) return EINVAL;

    return 0;
}

Error UdpNode::Recv_Locked(void* buf, size_t len, PP_Resource* out_addr, int* out_len) {
    Packet* packet = emitter_->ReadRXPacket_Locked();
    *out_len = 0;
    *out_addr = 0;

    if (packet) {
        int capped_len = static_cast<int32_t>(std::min<int>(len, packet->len()));
        memcpy(buf, packet->buffer(), capped_len);

        if (packet->addr() != 0) {
            filesystem_->ppapi()->AddRefResource(packet->addr());
            *out_addr = packet->addr();
        }

        *out_len = capped_len;
        delete packet;
        return 0;
    }

    return EBADF;
}

Error UdpNode::Send_Locked(const void* buf, size_t len, PP_Resource addr, int* out_len) {
    if (!IsBound()) {
        sockaddr_in bind_addr;
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = 0;
        memset(&bind_addr.sin_addr, 0, sizeof(bind_addr.sin_addr));
        Error err = Bind(reinterpret_cast<const struct sockaddr*>(&bind_addr), sizeof(bind_addr));
        if (err != 0) return err;
    }

    *out_len = 0;
    int capped_len = static_cast<int32_t>(std::min<int>(len, kMaxPacketSize));
    Packet* packet = new Packet(filesystem_->ppapi());
    packet->Copy(buf, capped_len, addr);

    emitter_->WriteTXPacket_Locked(packet);
    *out_len = capped_len;
    return 0;
}

Error UdpNode::SetSockOptSocket(int optname, const void* optval, socklen_t len) {
    if (static_cast<size_t>(len) < sizeof(int)) return EINVAL;

    switch (optname) {
        case SO_RCVBUF: {
            int bufsize = *static_cast<const int*>(optval);
            int32_t error = udp_interface_wrapper_->SetOption(
                socket_resource_, PP_UDPSOCKET_OPTION_RECV_BUFFER_SIZE, PP_MakeInt32(bufsize),
                PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        case SO_SNDBUF: {
            int bufsize = *static_cast<const int*>(optval);
            int32_t error = udp_interface_wrapper_->SetOption(
                socket_resource_, PP_UDPSOCKET_OPTION_SEND_BUFFER_SIZE, PP_MakeInt32(bufsize),
                PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        case SO_BROADCAST: {
            int broadcast = *static_cast<const int*>(optval);
            int32_t error = udp_interface_wrapper_->SetOption(
                socket_resource_, PP_UDPSOCKET_OPTION_BROADCAST,
                PP_MakeBool(broadcast ? PP_TRUE : PP_FALSE), PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        default: {
            break;
        }
    }

    return SocketNode::SetSockOptSocket(optname, optval, len);
}

Error UdpNode::SetSockOptIP(int optname, const void* optval, socklen_t len) {
    if (static_cast<size_t>(len) < sizeof(int)) return EINVAL;

    switch (optname) {
        case IP_MULTICAST_LOOP: {
            int loop = *static_cast<const int*>(optval);
            int32_t error = udp_interface_wrapper_->SetOption(
                socket_resource_, PP_UDPSOCKET_OPTION_MULTICAST_LOOP,
                PP_MakeBool(loop ? PP_TRUE : PP_FALSE), PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        case IP_MULTICAST_TTL: {
            unsigned int ttl = *static_cast<const unsigned int*>(optval);
            int32_t error = udp_interface_wrapper_->SetOption(
                socket_resource_, PP_UDPSOCKET_OPTION_MULTICAST_TTL, PP_MakeInt32(ttl),
                PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        case IP_MULTICAST_IF: {
            return 0;
        }
        case IP_ADD_MEMBERSHIP: {
            const struct ip_mreq* mreq = static_cast<const struct ip_mreq*>(optval);
            struct sockaddr_in sin = {};
            sin.sin_family = AF_INET;
            memcpy(&sin.sin_addr, &mreq->imr_multiaddr, sizeof(struct in_addr));

            PP_Resource net_addr = SockAddrInToResource(&sin, sizeof(sin));
            int32_t error = udp_interface_wrapper_->JoinGroup(socket_resource_, net_addr,
                                                              PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        case IP_DROP_MEMBERSHIP: {
            const struct ip_mreq* mreq = static_cast<const struct ip_mreq*>(optval);
            struct sockaddr_in sin = {};
            sin.sin_family = AF_INET;
            memcpy(&sin.sin_addr, &mreq->imr_multiaddr, sizeof(struct in_addr));

            PP_Resource net_addr = SockAddrInToResource(&sin, sizeof(sin));
            int32_t error = udp_interface_wrapper_->LeaveGroup(socket_resource_, net_addr,
                                                               PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        default: {
            break;
        }
    }

    return SocketNode::SetSockOptIP(optname, optval, len);
}

Error UdpNode::SetSockOptIPV6(int optname, const void* optval, socklen_t len) {
    if (static_cast<size_t>(len) < sizeof(int)) return EINVAL;

    switch (optname) {
        case IPV6_MULTICAST_LOOP: {
            int loop = *static_cast<const int*>(optval);
            int32_t error = udp_interface_wrapper_->SetOption(
                socket_resource_, PP_UDPSOCKET_OPTION_MULTICAST_LOOP,
                PP_MakeBool(loop ? PP_TRUE : PP_FALSE), PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        case IPV6_MULTICAST_HOPS: {
            unsigned int ttl = *static_cast<const unsigned int*>(optval);
            int32_t error = udp_interface_wrapper_->SetOption(
                socket_resource_, PP_UDPSOCKET_OPTION_MULTICAST_TTL, PP_MakeInt32(ttl),
                PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        case IPV6_MULTICAST_IF: {
            return 0;
        }
        case IPV6_JOIN_GROUP: {
            const struct ipv6_mreq* mreq = static_cast<const struct ipv6_mreq*>(optval);
            struct sockaddr_in6 sin = {};
            sin.sin6_family = AF_INET6;
            memcpy(&sin.sin6_addr, &mreq->ipv6mr_multiaddr, sizeof(struct in6_addr));

            PP_Resource net_addr = SockAddrIn6ToResource(&sin, sizeof(sin));
            int32_t error = udp_interface_wrapper_->JoinGroup(socket_resource_, net_addr,
                                                              PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        case IPV6_LEAVE_GROUP: {
            const struct ipv6_mreq* mreq = static_cast<const struct ipv6_mreq*>(optval);
            struct sockaddr_in6 sin = {};
            sin.sin6_family = AF_INET6;
            memcpy(&sin.sin6_addr, &mreq->ipv6mr_multiaddr, sizeof(struct in6_addr));

            PP_Resource net_addr = SockAddrIn6ToResource(&sin, sizeof(sin));
            int32_t error = udp_interface_wrapper_->LeaveGroup(socket_resource_, net_addr,
                                                               PP_BlockUntilComplete());
            return PPERROR_TO_ERRNO(error);
        }
        default: {
            break;
        }
    }

    return SocketNode::SetSockOptIPV6(optname, optval, len);
}

}  // namespace nacl_io
