FROM ubuntu:20.04 as base

ADD install-shared-deps.sh .
RUN bash ./install-shared-deps.sh

# set paths
ENV PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH
ENV LD_LIBRARY_PATH /usr/local/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:/lib32:/usr/lib32

####################################################################
# base image contains all the dependencies we need to build the code
FROM base

# get the code into the right place and prepare to build it
WORKDIR /usr/local/src/valhalla
ADD . /usr/local/src/valhalla
RUN ls
RUN git submodule sync && git submodule update --init --recursive
RUN mkdir build

# configure the build with symbols turned on so that crashes can be triaged
WORKDIR /usr/local/src/valhalla/build
RUN cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
RUN make all -j$(nproc)
RUN make install

# we wont leave the source around but we'll drop the commit hash we'll also keep the locales
WORKDIR /usr/local/src
RUN cd valhalla && echo "https://github.com/valhalla/valhalla/tree/$(git rev-parse HEAD)" > ../valhalla_version
RUN for f in valhalla/locales/*.json; do cat ${f} | python3 -c 'import sys; import json; print(json.load(sys.stdin)["posix_locale"])'; done > valhalla_locales
RUN rm -rf valhalla

# the binaries are huge with all the symbols so we strip them but keep the debug there if we need it
WORKDIR /usr/local/bin
RUN for f in valhalla_*; do objcopy --only-keep-debug $f $f.debug; done
RUN tar -cvf valhalla.debug.tar valhalla_*.debug && gzip -9 valhalla.debug.tar
RUN rm -f valhalla_*.debug
RUN strip --strip-debug --strip-unneeded valhalla_* || true
RUN strip /usr/local/lib/libvalhalla.a
RUN strip /usr/local/lib/python3.8/dist-packages/python_valhalla.cpython-38-x86_64-linux-gnu.so

####################################################################
# copy the important stuff from the build stage to the runner image
FROM ubuntu:20.04 as runner
MAINTAINER Kevin Kreiser <kevinkreiser@gmail.com>
COPY --from=builder /usr/local /usr/local
COPY --from=builder /usr/bin/prime_* /usr/bin/
COPY --from=builder /usr/lib/x86_64-linux-gnu/libprime* /usr/lib/x86_64-linux-gnu/

# we need to add back some runtime dependencies for binaries and scripts
# install all the posix locales that we support
RUN export DEBIAN_FRONTEND=noninteractive && apt update && \
    apt install -y \
      libboost-program-options1.71.0 libcurl4 libczmq4 libluajit-5.1-2 \
      libprotobuf-lite17 libsqlite3-0 libsqlite3-mod-spatialite libzmq5 zlib1g \
      curl gdb locales parallel python3.8-minimal python-is-python3 \
      spatialite-bin unzip wget && \
    cat /usr/local/src/valhalla_locales | xargs -d '\n' -n1 locale-gen && \
    rm -rf /var/lib/apt/lists/*
