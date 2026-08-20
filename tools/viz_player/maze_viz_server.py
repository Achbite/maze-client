#!/usr/bin/env python3
"""Serve deterministic evaluation replays."""

import argparse
import glob
import json
import mimetypes
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
from urllib.parse import parse_qs, urlparse


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_WEB_DIR = SCRIPT_DIR
ENTRY_PAGE = "maze_viz.html"


class VizReplayServer:
    """Indexes replay files while the Client may still be appending frames."""

    def __init__(self, replay_dir, validation_id, mode):
        self.replay_dir = os.path.abspath(replay_dir)
        self.validation_id = validation_id
        self.mode = mode
        self.lock = threading.Lock()
        self._index_cache = {}
        self._file_list_cache = None
        self._file_list_time = 0
        self._map_cache = {}
        self.maps_dir = os.path.join(self.replay_dir, "maps")
        self.validation_dir = self.replay_dir

        os.makedirs(self.replay_dir, exist_ok=True)
        print(f"[VizServer] 监控目录: {self.replay_dir}")

    def list_files(self):
        """列出回放目录下所有 .jsonl 文件，按修改时间倒序。"""
        now = time.time()
        if self._file_list_cache is not None and (now - self._file_list_time) < 2.0:
            return self._file_list_cache

        pattern = os.path.join(self.replay_dir, "*.jsonl")
        files = glob.glob(pattern)
        result = []

        for f in sorted(files, key=lambda x: os.path.getmtime(x), reverse=True):
            try:
                stat = os.stat(f)
                name = os.path.basename(f)
                line_offsets = self._build_line_index(name, f)
                episode_id = None
                map_id = ""
                if line_offsets:
                    with open(f, "r", encoding="utf-8") as stream:
                        stream.seek(line_offsets[0])
                        first_frame = json.loads(stream.readline())
                    episode_id = first_frame.get("episode_id")
                    map_id = first_frame.get("map_id", "")
                result.append({
                    "name": name,
                    "match_id": os.path.splitext(name)[0],
                    "episode_id": episode_id,
                    "map_id": map_id,
                    "frame_count": len(line_offsets),
                    "size_bytes": stat.st_size,
                    "modified_ts": stat.st_mtime,
                    "size_kb": round(stat.st_size / 1024, 1),
                    "modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime)),
                    "is_growing": (now - stat.st_mtime) < 10,
                })
            except (OSError, json.JSONDecodeError):
                pass

        self._file_list_cache = result
        self._file_list_time = now
        return result

    def _validate_filename(self, filename):
        """安全检查：防止路径穿越。返回 (filepath, error)。"""
        if '..' in filename or '/' in filename or '\\' in filename:
            return None, "非法文件名"
        filepath = os.path.join(self.replay_dir, filename)
        if not os.path.exists(filepath):
            return None, f"文件不存在: {filename}"
        return filepath, None

    def _build_line_index(self, filename, filepath):
        """构建或增量更新行偏移索引。返回 line_offsets 列表。
        
        使用二进制模式逐行读取 + 大缓冲区（1MB），对大文件性能显著优于默认缓冲。
        """
        try:
            current_mtime = os.path.getmtime(filepath)
            current_size = os.path.getsize(filepath)
        except OSError:
            return []

        with self.lock:
            cached = self._index_cache.get(filename)

            # 缓存命中且文件未变化
            if cached and cached["mtime"] == current_mtime and cached["size"] == current_size:
                return cached["line_offsets"]

            # 增量更新：文件增长时从上次位置继续扫描
            if cached and cached["size"] <= current_size:
                line_offsets = cached["line_offsets"][:]
                start_pos = cached["size"]
            else:
                # 全量重建
                line_offsets = []
                start_pos = 0

            try:
                # 使用 1MB 缓冲区加速大文件读取
                with open(filepath, 'rb', buffering=1048576) as f:
                    f.seek(start_pos)
                    offset = start_pos
                    for line in f:
                        if line.strip():
                            line_offsets.append(offset)
                        offset += len(line)
            except Exception as e:
                print(f"[VizServer] 构建索引失败 {filename}: {e}")
                return line_offsets

            # 更新缓存
            self._index_cache[filename] = {
                "mtime": current_mtime,
                "size": current_size,
                "line_offsets": line_offsets,
            }

            return line_offsets

    def get_frame_count(self, filename):
        """快速返回文件总帧数（不解析 JSON 内容）。"""
        filepath, error = self._validate_filename(filename)
        if error:
            return None, error

        line_offsets = self._build_line_index(filename, filepath)
        return len(line_offsets), None

    def load_frames_paged(self, filename, offset=0, limit=200):
        """分页加载帧数据。通过行偏移索引精准 seek，内存占用恒定。"""
        filepath, error = self._validate_filename(filename)
        if error:
            return None, 0, error

        line_offsets = self._build_line_index(filename, filepath)
        total = len(line_offsets)

        if offset >= total:
            return [], total, None

        # 计算实际读取范围
        end = min(offset + limit, total)
        frames = []

        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                for i in range(offset, end):
                    f.seek(line_offsets[i])
                    line = f.readline().strip()
                    if line:
                        try:
                            frames.append(json.loads(line))
                        except json.JSONDecodeError as e:
                            print(f"[VizServer] 跳过无效行 {filename}:line_{i}: {e}")
        except Exception as e:
            return None, total, f"读取文件失败: {e}"

        return frames, total, None

    def get_file_status(self, filename):
        """获取文件实时状态（用于 Live 播放模式轮询）。"""
        filepath, error = self._validate_filename(filename)
        if error:
            return None, error

        try:
            stat = os.stat(filepath)
        except OSError:
            return None, f"无法读取文件: {filename}"

        # 构建/更新行索引（增量更新，开销很小）
        line_offsets = self._build_line_index(filename, filepath)
        now = time.time()

        return {
            "file": filename,
            "total_lines": len(line_offsets),
            "size_bytes": stat.st_size,
            "is_growing": (now - stat.st_mtime) < 10,
            "last_modified": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime)),
        }, None

    def get_status(self):
        """获取服务状态。"""
        file_list = self.list_files()
        manifest = self._load_optional_json(
            os.path.join(
                self.validation_dir, "validation-manifest.json"
            )
        )
        client_result = self._load_optional_json(
            os.path.join(self.validation_dir, "client-result.json")
        )
        return {
            "schema_version": 1,
            "mode": self.mode,
            "validation_id": self.validation_id,
            "source": "client-replay",
            "model": manifest.get("model", {}),
            "parameters": manifest.get("parameters", {}),
            "client_result": client_result,
            "replay_dir": self.replay_dir,
            "file_count": len(file_list),
            "cached_files": len(self._index_cache),
            "cached_maps": len(self._map_cache),
        }

    @staticmethod
    def _load_optional_json(path):
        try:
            with open(path, "r", encoding="utf-8") as stream:
                return json.load(stream)
        except (OSError, json.JSONDecodeError):
            return {}

    def load_map(self, map_id):
        """加载地图 JSON 文件（从 maps/ 目录读取，内存缓存）。"""
        # 安全检查：防止路径穿越
        if '..' in map_id or '/' in map_id or '\\' in map_id:
            return None, "非法地图 ID"

        # 缓存命中
        if map_id in self._map_cache:
            return self._map_cache[map_id], None

        # 从磁盘加载
        map_path = os.path.join(self.maps_dir, f"{map_id}.json")
        if not os.path.exists(map_path):
            return None, f"地图文件不存在: {map_id}.json"

        try:
            with open(map_path, 'r', encoding='utf-8') as f:
                map_data = json.load(f)
            # 缓存到内存
            self._map_cache[map_id] = map_data
            print(f"[VizServer] 地图已加载并缓存: {map_id}")
            return map_data, None
        except Exception as e:
            return None, f"加载地图失败: {e}"


viz_server = None
web_dir = DEFAULT_WEB_DIR


class VizHTTPHandler(BaseHTTPRequestHandler):
    """Serves replay data and the local validation page."""

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        params = parse_qs(parsed.query)

        if path in ('/', '/index.html'):
            self._serve_html()

        elif path.startswith('/assets/'):
            self._serve_asset(path.removeprefix('/assets/'))

        elif path in ('/api/files', '/api/client/files'):
            file_list = viz_server.list_files()
            if path == '/api/files':
                self._json_response(file_list)
            else:
                self._json_response({
                    "schema_version": 1,
                    "mode": viz_server.mode,
                    "validation_id": viz_server.validation_id,
                    "source": "client-replay",
                    "files": file_list,
                })

        elif path in ('/api/frames', '/api/client/frames'):
            filename = params.get('file', [None])[0]
            if not filename:
                self._json_response({"error": "缺少 file 参数"}, status=400)
                return

            # 分页参数（默认 offset=0, limit=200）
            try:
                offset = int(params.get('offset', ['0'])[0])
                limit = int(params.get('limit', ['200'])[0])
            except (ValueError, IndexError):
                offset, limit = 0, 200

            # 限制单次最大返回量
            limit = min(limit, 2000)

            frames, total, error = viz_server.load_frames_paged(filename, offset, limit)
            if error:
                self._json_response({"error": error}, status=404)
            else:
                self._json_response({
                    "file": filename,
                    "total_frames": total,
                    "offset": offset,
                    "limit": limit,
                    "count": len(frames),
                    "frames": frames,
                })

        elif path == '/api/frame_count':
            filename = params.get('file', [None])[0]
            if not filename:
                self._json_response({"error": "缺少 file 参数"}, status=400)
                return
            count, error = viz_server.get_frame_count(filename)
            if error:
                self._json_response({"error": error}, status=404)
            else:
                self._json_response({"file": filename, "total_frames": count})

        elif path == '/api/file_status':
            filename = params.get('file', [None])[0]
            if not filename:
                self._json_response({"error": "缺少 file 参数"}, status=400)
                return
            status, error = viz_server.get_file_status(filename)
            if error:
                self._json_response({"error": error}, status=404)
            else:
                self._json_response(status)

        elif path in ('/api/map', '/api/client/map'):
            map_id = params.get('id', [None])[0]
            if not map_id:
                self._json_response({"error": "缺少 id 参数"}, status=400)
                return
            map_data, error = viz_server.load_map(map_id)
            if error:
                self._json_response({"error": error}, status=404)
            else:
                self._json_response(map_data)

        elif path in ('/api/status', '/api/client/status'):
            self._json_response(viz_server.get_status())

        else:
            self.send_error(404, "Not Found")

    def do_OPTIONS(self):
        """处理 CORS 预检请求。"""
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.send_header('Content-Length', '0')
        self.end_headers()

    def _json_response(self, data, status=200):
        """发送 JSON 响应。"""
        try:
            body = json.dumps(data, ensure_ascii=False, separators=(',', ':'))
            body_bytes = body.encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Content-Length', len(body_bytes))
            self.end_headers()
            self.wfile.write(body_bytes)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _serve_html(self):
        html_path = os.path.join(web_dir, ENTRY_PAGE)
        if not os.path.exists(html_path):
            self.send_error(404, f"{ENTRY_PAGE} not found")
            return
        try:
            with open(html_path, 'rb') as f:
                content = f.read()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', len(content))
            self.end_headers()
            self.wfile.write(content)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _serve_asset(self, relative_path):
        if (
            not relative_path
            or '..' in relative_path
            or '/' in relative_path
            or '\\' in relative_path
        ):
            self.send_error(404, "Not Found")
            return
        path = os.path.join(web_dir, relative_path)
        try:
            with open(path, 'rb') as stream:
                content = stream.read()
        except OSError:
            self.send_error(404, "Not Found")
            return
        content_type = mimetypes.guess_type(path)[0] or 'application/octet-stream'
        if content_type.startswith('text/') or content_type == 'application/javascript':
            content_type += '; charset=utf-8'
        try:
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', len(content))
            self.end_headers()
            self.wfile.write(content)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def log_message(self, format, *args):
        """过滤常规 200 请求日志，只打印异常请求。"""
        if '200' not in str(args):
            super().log_message(format, *args)


def main():
    global viz_server, web_dir

    parser = argparse.ArgumentParser(
        description='Local model-validation replay server'
    )
    parser.add_argument('--dir', '-d', default='log/viz',
                        help='回放文件目录（默认: log/viz）')
    parser.add_argument('--host', default='127.0.0.1',
                        help='HTTP 监听地址（默认: 127.0.0.1）')
    parser.add_argument('--port', '-p', type=int, default=9004,
                        help='HTTP 服务端口（默认: 9004）')
    parser.add_argument('--validation-id', default='legacy-local-validation',
                        help='本地模型验证任务 ID')
    parser.add_argument(
        '--mode',
        choices=('evaluation',),
        default='evaluation',
    )
    parser.add_argument('--web-dir', default=DEFAULT_WEB_DIR,
                        help='本地验证页面静态文件目录')
    args = parser.parse_args()

    web_dir = os.path.abspath(args.web_dir)
    viz_server = VizReplayServer(args.dir, args.validation_id, args.mode)

    file_list = viz_server.list_files()
    print(f"[VizServer] 发现 {len(file_list)} 个回放文件")

    class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    server = ThreadingHTTPServer((args.host, args.port), VizHTTPHandler)
    print(
        f"[VizServer] 本地验证回放服务已启动: "
        f"http://{args.host}:{args.port}"
    )

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[VizServer] 服务已停止")
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
