# syntax=docker/dockerfile:1
FROM node:current-bookworm-slim
ADD __learn__/bazel.pem /usr/local/share/ca-certificates/bazel.crt
ADD __learn__/pypi.pem /usr/local/share/ca-certificates/pypi.crt
RUN apt update -y && \
  apt install -y apt-transport-https ca-certificates && \
  dpkg-reconfigure ca-certificates && \
  echo "Installed PKI components" && \
  apt install -y python3-dev python3-pip python3.11-venv && \
  echo "Installed TF package deps" && \
  apt install -y llvm-16 clang-16 && \
  echo "Installed Clang compiler"
RUN npm i -g @bazel/bazelisk
USER node
WORKDIR /buildmachine
COPY . .
RUN python3 -m venv .venv && \
  source /buildmachine/.venv/bin/activate && \
  python3 -m pip install -U --trusted-host pypi.org --trusted-host files.pythonhosted.org truststore pip numpy wheel packaging requests opt_einsum && \
  python3 -m pip install -U --trusted-host pypi.org --trusted-host files.pythonhosted.org keras_preprocessing --no-deps && \
  ./configure && \
  $(which bazelisk) build --config=monolithic //tensorflow/tools/pip_package:build_pip_package
SHELL ["$(which bash)", "-c"]