FROM node:current-bookworm-slim
WORKDIR /buildmachine
RUN npm i -g @bazel/bazelisk
USER node
COPY . .
RUN $(which bazelisk) build
SHELL ["$(which bash)", "-c"]