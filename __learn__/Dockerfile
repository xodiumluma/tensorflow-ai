# syntax=docker/dockerfile:1
FROM node:current-bookworm-slim
WORKDIR /buildmachine
RUN npm i -g @bazel/bazelisk
ADD __learn__/bcr-bazel-build.pem /usr/local/share/ca-certificates/bazel.crt
RUN apt update -y && \
  apt install -y apt-transport-https ca-certificates && \
  dpkg-reconfigure ca-certificates
USER node
COPY . .
RUN $(which bazelisk) build 
SHELL ["$(which bash)", "-c"]