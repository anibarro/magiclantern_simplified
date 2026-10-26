#!/usr/bin/env python3

import os
import argparse
import sys
import subprocess
import shutil


def main():
    args = parse_args()
    if shutil.which("git"):
        # Only the revision and branch are needed. Avoid walking every untracked
        # camera build, ROM dump and card image just to identify the repository.
        repository = subprocess.run(["git", "rev-parse", "--is-inside-work-tree"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if repository.returncode != 0 or repository.stdout.strip() != "true":
            print("unofficial: not a git repo", end='')
        else:
            short_hash = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"]).strip().decode("utf8")
            branch = subprocess.check_output(["git", "branch", "--show-current"]).strip().decode("utf8")
            print(short_hash + " " + branch, end='')
    else:
        print("unofficial: no git", end='')


def parse_args():
    description = """
    Used during ML build to get git summary information,
    for use in version.c.  ML can display this info to the user.
    Similar info to "hg id".
    """

    parser = argparse.ArgumentParser(description=description)
    args = parser.parse_args()
    return args


if __name__ == "__main__":
    main()
