FROM python:3.12-slim AS base

COPY --from=ghcr.io/astral-sh/uv:latest /uv /usr/local/bin/uv

WORKDIR /app
ENV UV_LINK_MODE=copy UV_COMPILE_BYTECODE=1 PYTHONUNBUFFERED=1

# Dependencies resolve in their own layer so source edits do not re-resolve them.
COPY pyproject.toml ./
RUN uv venv /venv && VIRTUAL_ENV=/venv uv pip install -r pyproject.toml

COPY src ./src
ENV PATH="/venv/bin:$PATH" PYTHONPATH=/app/src

RUN useradd --create-home --uid 1000 bridge && mkdir -p /data && chown bridge:bridge /data
USER bridge
VOLUME /data
EXPOSE 8090

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD python -c "import urllib.request;urllib.request.urlopen('http://127.0.0.1:8090/health').read()"

CMD ["uvicorn", "bridge.main:app", "--host", "0.0.0.0", "--port", "8090"]
