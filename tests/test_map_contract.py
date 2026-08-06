#!/usr/bin/env python3

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import unittest


REPOSITORY = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parents[1])
GENERATOR_PATH = REPOSITORY / "tools/map_generator/generate_maze.py"
FIXED_MAP_PATH = REPOSITORY / "maps/test/maze_117436372.json"

spec = importlib.util.spec_from_file_location("maze_generator", GENERATOR_PATH)
generator = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(generator)


class MapContractTest(unittest.TestCase):
    def test_fixed_map_v4_identity_and_shortest_path(self):
        data = json.loads(FIXED_MAP_PATH.read_text(encoding="utf-8"))
        self.assertEqual(data["version"], 4)
        self.assertEqual(data["action_rule_id"], generator.ACTION_RULE_ID)

        cols = data["grid_cols"]
        rows = data["grid_rows"]
        bitmap = bytes.fromhex(data["blocked_bitmap_hex"])
        self.assertEqual(len(bitmap), cols * rows)
        self.assertTrue(all(value in (0, 1) for value in bitmap))
        blocked = [
            [bool(bitmap[y * cols + x]) for x in range(cols)]
            for y in range(rows)
        ]
        start = data["start_grid"]
        goal = data["goal_grid"]
        self.assertFalse(blocked[start["y"]][start["x"]])
        self.assertFalse(blocked[goal["y"]][goal["x"]])

        shortest = generator.shortest_action_steps(
            blocked, cols, rows,
            start["x"], start["y"], goal["x"], goal["y"],
        )
        self.assertEqual(shortest, 188)
        self.assertEqual(data["shortest_action_steps"], shortest)
        self.assertEqual((8 * shortest, 4 * shortest, 2 * shortest),
                         (1504, 752, 376))

        checksum = generator.canonical_map_checksum(
            cols, rows, data["grid_size"],
            start["x"], start["y"], goal["x"], goal["y"],
            bitmap, data["action_rule_id"],
        )
        self.assertEqual(checksum, data["checksum_sha256"])

    def test_diagonal_cannot_cut_a_blocked_corner(self):
        blocked = [[False, True], [True, False]]
        self.assertEqual(
            generator.shortest_action_steps(blocked, 2, 2, 0, 0, 1, 1),
            -1,
        )

    def test_checksum_is_independent_of_json_formatting(self):
        data = json.loads(FIXED_MAP_PATH.read_text(encoding="utf-8"))
        bitmap = bytes.fromhex(data["blocked_bitmap_hex"])
        start = data["start_grid"]
        goal = data["goal_grid"]
        canonical = generator.canonical_map_payload(
            data["grid_cols"], data["grid_rows"], data["grid_size"],
            start["x"], start["y"], goal["x"], goal["y"], bitmap,
            data["action_rule_id"],
        )
        self.assertEqual(hashlib.sha256(canonical).hexdigest(),
                         data["checksum_sha256"])
        reparsed = json.loads(json.dumps(data, sort_keys=True, indent=None))
        self.assertEqual(reparsed["checksum_sha256"], data["checksum_sha256"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
