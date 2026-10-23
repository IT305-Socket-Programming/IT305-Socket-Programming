#!/usr/bin/env python3
"""
package_submission.py - Packages the IT305 Final Submission Archive (.zip and .tar.gz)
Strictly adheres to assignment structure:
  FinalSubmission_Group_<XXX>_<YYY>/
    FinalReport_Group_<XXX>_<YYY>.pdf
    README.md
    Part1/
    Part2_Case1/
    Part2_Case2/
    Part2_Case2_Enhanced/
    Results/
"""
import os
import sys
import zipfile
import tarfile
from pathlib import Path

def package(group_x="109", group_y="104", output_dir="."):
    root = Path(__file__).resolve().parent
    archive_name = f"FinalSubmission_Group_{group_x}_{group_y}"
    report_filename = f"FinalReport_Group_{group_x}_{group_y}.pdf"
    
    # Verify required report exists
    report_path = root / report_filename
    if not report_path.is_file():
        raise FileNotFoundError(f"Required report {report_filename} not found at {root}")
    
    # Ensure all required folders exist
    parts = ["Part1", "Part2_Case1", "Part2_Case2", "Part2_Case2_Enhanced", "Results"]
    for p in parts:
        if not (root / p).is_dir():
            raise FileNotFoundError(f"Required directory {p} not found at {root}")

    # Files to exclude from the archive (temporary, object files, binaries, git metadata)
    exclude_extensions = {".o", ".pyc", ".DS_Store", ".tmp", ".log"}
    exclude_dirs = {".git", ".staging", "__pycache__"}
    exclude_files = {"server", "client"}  # Clean source submission, evaluator runs make

    zip_path = Path(output_dir) / f"{archive_name}.zip"
    tar_path = Path(output_dir) / f"{archive_name}.tar.gz"

    print(f"Creating ZIP archive: {zip_path}")
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        # Add FinalReport
        arc_report = f"{archive_name}/{report_filename}"
        z.write(report_path, arc_report)

        # Add README.md
        readme_path = root / "README.md"
        arc_readme = f"{archive_name}/README.md"
        z.write(readme_path, arc_readme)

        # Add case directories and Results
        for folder in parts:
            folder_path = root / folder
            for p in sorted(folder_path.rglob("*")):
                # Check directory exclusion
                if any(ex in p.parts for ex in exclude_dirs):
                    continue
                if p.is_file():
                    if p.suffix in exclude_extensions or p.name in exclude_files:
                        continue
                    # Compute relative path using POSIX forward slashes
                    rel = p.relative_to(root).as_posix()
                    arcname = f"{archive_name}/{rel}"
                    z.write(p, arcname)

    print(f"Creating TAR.GZ archive: {tar_path}")
    with tarfile.open(tar_path, "w:gz") as t:
        arc_report = f"{archive_name}/{report_filename}"
        t.add(report_path, arcname=arc_report)

        arc_readme = f"{archive_name}/README.md"
        t.add(readme_path, arcname=arc_readme)

        for folder in parts:
            folder_path = root / folder
            for p in sorted(folder_path.rglob("*")):
                if any(ex in p.parts for ex in exclude_dirs):
                    continue
                if p.is_file():
                    if p.suffix in exclude_extensions or p.name in exclude_files:
                        continue
                    rel = p.relative_to(root).as_posix()
                    arcname = f"{archive_name}/{rel}"
                    t.add(p, arcname=arcname)

    print(f"Archives successfully created:\n  - {zip_path} ({zip_path.stat().st_size:,} bytes)\n  - {tar_path} ({tar_path.stat().st_size:,} bytes)")
    return zip_path, tar_path

if __name__ == "__main__":
    gx = sys.argv[1] if len(sys.argv) > 1 else "109"
    gy = sys.argv[2] if len(sys.argv) > 2 else "104"
    package(gx, gy)
