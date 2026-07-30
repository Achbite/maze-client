# Maze Client

English | [简体中文](README.md)

C++ maze environment client. It connects to AIServer and executes episodes. Inference and model-evaluation modes expose local replay on `9004`; training mode does not start replay.

## Quick Start

Build the image:

```bash
CLIENT_IMAGE_TAG=training-001 bash build_image.sh
```

Enter the development container and start the inference smoke test:

```bash
make shell
bash ./run.sh inference-smoke
```

Start training mode:

```bash
bash ./run.sh training
```

View previously recorded replay files:

```bash
bash ./replay.sh inference-smoke
```

Browser URL:

```text
http://127.0.0.1:9004/
```

## Tests

```bash
make test
```

## License

[MIT License](LICENSE)
