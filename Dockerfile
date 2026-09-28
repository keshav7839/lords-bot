FROM python:3.11-slim
WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libffi-dev && rm -rf /var/lib/apt/lists/*
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY . .
RUN cd bot \
    && mkdir -p data \
    && gcc -O2 -std=gnu11 -DVERSION='"1.0.2"' -Iinclude \
       src/main.c src/connection.c src/log.c src/protocol.c \
       src/des.c src/map_point.c src/command.c src/config.c src/auto.c \
       src/waves.c \
       -o client
CMD ["python", "app.py"]
