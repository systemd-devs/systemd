#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

from argparse import ArgumentParser
import glob
import json
import os
import re
import subprocess
import sys

import requests

BASE_URL = "https://www.freedesktop.org/software/systemd/man/"
JQUERY_URL = "https://code.jquery.com/jquery-3.7.1.min.js"
SCRIPT_TAG = '<script src="{}"></script>'

NAV_JS = """
$(document).ready(function() {
    $.getJSON("../index.json", function(data) {
        var versions = Object.keys(data).sort().reverse();

        var [filename, dirname] = window.location.pathname.split("/").reverse();

        var items = [];
        $.each(versions, function(_, version) {
            if (filename != "" && !data[version].includes(filename))
                return;

            if (version == dirname) {
                items.push( "<option selected value='" + version + "'>" + "systemd " + version + "</option>");
            } else if (dirname == "latest" && version == versions[0]) {
                items.push( "<option selected value='" + version + "'>" + "systemd " + version + "</option>");
            } else {
                items.push( "<option value='" + version + "'>" + "systemd " + version + "</option>");
            }
        });

        $("span:first").html($( "<select/>", {
            id: "version-selector",
            html: items.join( "" )
        }));

        $("#version-selector").on("change", function() {
            window.location.assign("../" + $(this).val() + "/" + filename);
        });
    });
});
"""


def process_file(filename):
    with open(filename) as f:
        contents = f.read()

    if SCRIPT_TAG.format("../nav.js") in contents:
        return

    body_tag = re.search("<body[^>]*>", contents)
    new_contents = (
        contents[: body_tag.end()]
        + SCRIPT_TAG.format(JQUERY_URL)
        + SCRIPT_TAG.format("../nav.js")
        + contents[body_tag.end() :]
    )

    with open(filename, "w") as f:
        f.write(new_contents)


def update_index_file(version, filenames, index_filename):
    response = requests.get(BASE_URL + "index.json")
    if response.status_code == 404:
        index = {}
    elif response.ok:
        index = response.json()
    else:
        sys.exit(f"Error getting index: {response.status_code} {response.reason}")

    index[version] = filenames

    with open(index_filename, "w") as f:
        json.dump(index, f)


def get_latest_version():
    tags = subprocess.check_output(["git", "tag", "-l", "v*"], text=True).split()
    versions = []
    for tag in tags:
        m = re.match("v?(\d+).*", tag)
        if m:
            versions.append(int(m.group(1)))
    return max(versions)


def main(version, directory, www_target):
    index_filename = os.path.join(directory, "index.json")
    nav_filename = os.path.join(directory, "nav.js")

    filenames = glob.glob("*.html", root_dir=directory)
    for filename in filenames:
        process_file(os.path.join(directory, filename))

    with open(nav_filename, "w") as f:
        f.write(NAV_JS)

    update_index_file(version, filenames, index_filename)

    dirs = [version]

    if int(version) == get_latest_version():
        dirs.append("latest")

    for d in dirs:
        subprocess.check_call(
            [
                "rsync",
                "-rlv",
                "--delete-excluded",
                "--include=*.html",
                "--exclude=*",
                "--omit-dir-times",
                directory + "/",  # copy contents of directory
                os.path.join(www_target, "man", d),
            ]
        )

    subprocess.check_call(
        [
            "rsync",
            "-v",
            os.path.join(directory, "index.json"),
            os.path.join(directory, "nav.js"),
            os.path.join(www_target, "man"),
        ]
    )


if __name__ == "__main__":
    parser = ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("directory")
    parser.add_argument("www_target")

    args = parser.parse_args()
    main(args.version, args.directory, args.www_target)
