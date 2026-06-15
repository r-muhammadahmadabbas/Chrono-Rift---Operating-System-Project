FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install core build tools and all supported GUI libraries
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    gdb \
    libsfml-dev \
    libsdl2-dev \
    libglfw3-dev \
    libncurses-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy and install any extra packages declared in requirements.txt
COPY requirements.txt /tmp/requirements.txt
RUN grep -v '^#' /tmp/requirements.txt | grep -v '^$' | \
    xargs -r apt-get install -y && rm -rf /var/lib/apt/lists/*

WORKDIR /app

CMD ["bash"]
