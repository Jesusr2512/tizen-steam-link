# Toolchain image for building the Tizen 3.0 NaCl (.wgt) package:
#  - Samsung NaCl SDK (pepper_63: PNaCl toolchain + PPAPI headers/libs)
#  - Tizen Studio CLI 5.6 (for signing and packaging .wgt)
FROM --platform=linux/amd64 ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture i386 && \
    apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        bzip2 \
        ca-certificates \
        cmake \
        cpio \
        curl \
        expect \
        file \
        git \
        libc6:i386 \
        libncurses5:i386 \
        libstdc++6:i386 \
        libz1:i386 \
        ninja-build \
        openjdk-8-jre-headless \
        patch \
        python2 \
        python3 \
        rpm2cpio \
        unzip \
        xz-utils \
        zip \
    && ln -sf /usr/bin/python2 /usr/bin/python \
    && rm -rf /var/lib/apt/lists/*

# Samsung NaCl SDK (pepper_63) for Tizen 3.0+ Smart TVs
RUN mkdir -p /nacl_sdk \
    && curl -kfsSL "https://developer.samsung.com/smarttv/file/55e181c4-fb5e-4d25-82df-f85b3b864dab" \
        -o /tmp/nacl_sdk.zip \
    && unzip -q /tmp/nacl_sdk.zip -d /nacl_sdk \
    && rm -f /tmp/nacl_sdk.zip \
    && useradd -m -s /bin/bash tsl

ENV NACL_SDK_ROOT=/nacl_sdk/pepper_63
ENV CMAKE_TLS_VERIFY=0

USER tsl
WORKDIR /home/tsl

RUN curl -kfsSL "https://download.tizen.org/sdk/Installer/tizen-studio_5.6/web-cli_Tizen_Studio_5.6_ubuntu-64.bin" \
        -o web-cli.bin \
    && chmod +x web-cli.bin \
    && ./web-cli.bin --accept-license /home/tsl/tizen-studio \
    && rm web-cli.bin
ENV PATH=/home/tsl/tizen-studio/tools/ide/bin:/home/tsl/tizen-studio/tools:${PATH}

RUN tizen certificate -a TSL -f TSL -p 1234 \
    && tizen security-profiles add -n TSL -a /home/tsl/tizen-studio-data/keystore/author/TSL.p12 -p 1234 \
    && sed -i 's|/home/tsl/tizen-studio-data/keystore/author/TSL.pwd||' \
        /home/tsl/tizen-studio-data/profile/profiles.xml \
    && sed -i 's|/home/tsl/tizen-studio-data/tools/certificate-generator/certificates/distributor/tizen-distributor-signer.pwd|tizenpkcs12passfordsigner|' \
        /home/tsl/tizen-studio-data/profile/profiles.xml

WORKDIR /src
