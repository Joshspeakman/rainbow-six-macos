# r6relay in a container (Fly.io builds this remotely: no local Docker needed). Standard library only.
FROM python:3.13-slim
COPY r6relay.py /app/r6relay.py
USER nobody
EXPOSE 2346
# --proxy-protocol: behind Fly's proxy_proto handler (fly.toml), each connection starts with the player's own address
CMD ["python3", "-u", "/app/r6relay.py", "--port", "2346", "--proxy-protocol"]
