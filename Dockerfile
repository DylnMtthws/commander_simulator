FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        git \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DCS_WERROR=ON \
    && cmake --build /build --target cs \
    && strip /build/src/cli/cs


FROM python:3.13-slim-bookworm AS python

ARG UV_VERSION=0.12.8
RUN pip install --no-cache-dir "uv==${UV_VERSION}"

WORKDIR /src
COPY export ./export
COPY service ./service
RUN UV_PROJECT_ENVIRONMENT=/opt/export \
        uv sync --project export --frozen --no-dev --no-editable \
    && UV_PROJECT_ENVIRONMENT=/opt/service \
        uv sync --project service --frozen --no-dev --no-editable


FROM python:3.13-slim-bookworm AS runtime

ENV PATH="/opt/service/bin:${PATH}" \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app
COPY --from=builder /build/src/cli/cs /app/cs
COPY --from=python /opt/export /opt/export
COPY --from=python /opt/service /opt/service
COPY data /app/data

USER 1001:1001
EXPOSE 8080
ENTRYPOINT ["mtgsim-serve"]
