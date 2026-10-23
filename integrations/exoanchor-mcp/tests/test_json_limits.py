import json
import unittest

from exoanchor_mcp.json_limits import MAX_JSON_DEPTH, loads_bounded_json


class JsonLimitsTests(unittest.TestCase):
    def test_container_depth_boundary_for_arrays_objects_and_mixed_values(self):
        for opening, closing in (("[", "]"), ('{"x":', "}"), ('{"x":[', "]}")):
            step = 2 if opening.endswith("[") and opening.startswith("{") else 1
            for depth in (MAX_JSON_DEPTH - step, MAX_JSON_DEPTH, MAX_JSON_DEPTH + step):
                text = opening * (depth // step) + "null" + closing * (depth // step)
                with self.subTest(opening=opening, depth=depth):
                    if depth > MAX_JSON_DEPTH:
                        with self.assertRaisesRegex(ValueError, "JSON nesting exceeds"):
                            loads_bounded_json(text)
                    else:
                        self.assertEqual(loads_bounded_json(text), json.loads(text))

    def test_null_and_scalar_siblings_do_not_hide_deep_values(self):
        for first in ("null", "false", "0", '"text"', "[]", "{}"):
            text = "[" + first + "," + "[" * MAX_JSON_DEPTH + "0" + "]" * MAX_JSON_DEPTH + "]"
            with self.subTest(first=first), self.assertRaises(ValueError):
                loads_bounded_json(text)

    def test_strings_width_and_existing_byte_encodings_are_preserved(self):
        value = {"escaped": '\\"[{]}\\' * 100, "unicode": "测试", "wide": [None, 0, {}] * 1000}
        text = json.dumps(value, ensure_ascii=False)
        for content in (text, text.encode("utf-8"), text.encode("utf-16"), text.encode("utf-32")):
            self.assertEqual(loads_bounded_json(content), value)
        for scalar in (None, False, 1, "text"):
            self.assertEqual(loads_bounded_json(json.dumps(scalar)), scalar)

    def test_syntax_errors_remain_errors(self):
        for text in ('{"x":}', "[}", "null false", '"unterminated', b"\xff"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                loads_bounded_json(text)


if __name__ == "__main__":
    unittest.main()
