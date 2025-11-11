# Pluto SDR Docker images

This repository contains the Dockerfiles. Docker images are published as GHCR.

## plutosdr-devel

[ghcr.io/hyojun01/plutosdr-devel](https://github.com/orgs/hyojun01/packages/container/package/plutosdr-devel)

Additionally, the image has all the requirements to run
[Vivado](https://www.xilinx.com/products/design-tools/vivado.html) 2023.2, which
is needed to build the FPGA bitstream. Vivado needs to be installed manually on
a volume as described below.

The docker image can be run as follows:
```
docker run --rm --net host -e DISPLAY=$DISPLAY -e TERM \
    --name=ws-sdr-devel --hostname=ws-sdr-devel \
    -v vivado2023_2:/opt/Xilinx -v ws_sdr_devel_home:/home \
    -v $HOME/pluto_ws:/hdl \
    --ulimit "nofile=1024:1048576" \
    -it ghcr.io/hyojun01/plutosdr-devel
```

This assumes that Vivado has been installed to a Docker volume
`vivado2023_2` and uses a volume to hold the home directory (in order to
have persistent bash history, etc.). It mounts the user home directory into
`/hdl`, so that the Maia SDR repositories working copies can be accessed
(different paths can be used here.).

The home of the docker container user should contain the following in `.bashrc`:
```
source /opt/Xilinx/Vivado/2023.2/settings64.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin/:/usr/bin:/sbin:/bin
```

In order to install Vivado to the volume, the installer can be run as root. A
root bash session can be launched in the container by doing
```
docker exec -u 0 -it ws-sdr-devel /bin/bash
```
From this session, it is possible to run the Vivado installer and choose
`/opt/Xilinx` as the installation path.
