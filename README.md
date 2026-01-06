# openautoflutter

A new Flutter plugin project.

## Docker build environment

Build an isolated Linux toolchain with Flutter, clang-18, and libc++ using the provided `Dockerfile`:

```
docker build -t openautoflutter-builder .
```

Run builds inside the container while mounting the workspace (example builds the example app for Linux):

```
docker run --rm -it \
	-v "$PWD:/workspace" \
	-w /workspace/example \
	openautoflutter-builder \
	flutter build linux -v
```

The image precaches Linux artifacts and enables the Linux desktop target; no extra setup is required in the container.

## Getting Started

This project is a starting point for a Flutter
[plug-in package](https://flutter.dev/to/develop-plugins),
a specialized package that includes platform-specific implementation code for
Android and/or iOS.

For help getting started with Flutter development, view the
[online documentation](https://docs.flutter.dev), which offers tutorials,
samples, guidance on mobile development, and a full API reference.

