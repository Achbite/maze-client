# Maze Client

简体中文 | [English](README.en.md)

C++ 迷宫环境客户端。它连接 AIServer 执行 Episode；推理与模型验证模式会在 `9004` 提供本地回放，训练模式不启动回放。

## 快速开始

构建镜像：

```bash
CLIENT_IMAGE_TAG=training-001 bash build_image.sh
```

进入开发容器并启动推理测试：

```bash
make shell
bash ./run.sh inference-smoke
```

启动训练模式：

```bash
bash ./run.sh training
```

单独查看已经保存的回放：

```bash
bash ./replay.sh inference-smoke
```

浏览器地址：

```text
http://127.0.0.1:9004/
```

## 测试

```bash
make test
```

## License

[MIT License](LICENSE)
