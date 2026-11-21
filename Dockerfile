FROM python:3.12-bookworm@sha256:3c3ba8ad4c85eb63b7568f806434f14e33370056985da7fdf2673621cb1d7e93 AS build
WORKDIR /src
RUN pip install --no-cache-dir --no-deps pyarrow==19.0.1
COPY main.cpp pipeline.hpp input.hpp output.hpp keys.hpp dates.hpp csv.hpp build.sh ./
RUN set -eu; \
    mkdir -p /rootfs/app/lib /rootfs/tmp /rootfs/dev /rootfs/proc /rootfs/scratch /rootfs/output /rootfs/data; \
    sh build.sh /rootfs/app/shopping-sessions; \
    arrow_dir="$(python -c 'from importlib.util import find_spec; from pathlib import Path; print(Path(find_spec("pyarrow").origin).parent)')"; \
    cp "$arrow_dir/libarrow.so.1900" "$arrow_dir/libparquet.so.1900" /rootfs/app/lib/; \
    ldd /rootfs/app/shopping-sessions > /tmp/needed; \
    if grep -q 'not found' /tmp/needed; then cat /tmp/needed >&2; exit 1; fi; \
    awk '/=> \/(lib|usr\/lib)\// {print $3}' /tmp/needed | \
        xargs -r -I '{}' cp -L --parents '{}' /rootfs; \
    cp -L --parents /lib64/ld-linux-x86-64.so.2 /rootfs; \
    chmod 1777 /rootfs/tmp; chown 1000:1000 /rootfs/scratch /rootfs/output
FROM scratch
COPY --from=build /rootfs/ /
ENV DATA_DIR=/data OUTPUT_DIR=/output SCRATCH_DIR=/scratch HOME=/scratch NCPU=8 MEM_GB=24
USER 1000:1000
ENTRYPOINT ["/app/shopping-sessions"]
