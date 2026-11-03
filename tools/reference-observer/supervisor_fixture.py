"""Subprocess-only synthetic collector for supervisor verification."""

from unittest.mock import patch

import observer


def main():
    original = observer.publish

    def synthetic(root, binding, scenario, records, provenance):
        return original(root, binding, scenario, records, "synthetic_fixture")

    with (
        patch("observer.sys.platform", "linux"),
        patch("observer.inventory", return_value={}),
        patch("observer.publish", side_effect=synthetic),
    ):
        observer.main()


if __name__ == "__main__":
    main()
