"""Install the SisFall dataset into data/SisFall_dataset.

SisFall (Sucerquia et al., Sensors 2017) is free for research, but the
original host often goes offline, so this script does not hard-code a URL.
Get the zip from the authors' page or a mirror (search "SisFall dataset zip"),
then:

    python ml/download_sisfall.py path/to/SisFall_dataset.zip
or  python ml/download_sisfall.py https://<mirror>/SisFall_dataset.zip
"""
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

import config as C


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    if src.startswith(("http://", "https://")):
        tmp = Path(tempfile.mkdtemp()) / "sisfall.zip"
        print(f"downloading {src} ...")
        urllib.request.urlretrieve(src, tmp)
        src = tmp
    src = Path(src)

    C.DATA_DIR.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(src) as z:
        for member in z.namelist():   # refuse path traversal in the archive
            target = (C.DATA_DIR / member).resolve()
            if not str(target).startswith(str(C.DATA_DIR.resolve())):
                sys.exit(f"unsafe path in zip: {member}")
        z.extractall(C.DATA_DIR)

    files = list(C.DATA_DIR.rglob("*_S[AE][0-9][0-9]_R[0-9][0-9].txt"))
    if not files:
        sys.exit("No SisFall trial files found after extraction.")
    root = files[0].parent.parent
    if root != C.SISFALL_DIR:
        print(f"Note: dataset extracted to {root}. preprocess.py looks in "
              f"{C.SISFALL_DIR}; rename the folder or edit SISFALL_DIR.")
    subjects = {f.parent.name for f in files}
    print(f"OK: {len(files)} trials from {len(subjects)} subjects.")


if __name__ == "__main__":
    main()
