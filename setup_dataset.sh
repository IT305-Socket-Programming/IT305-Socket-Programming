#!/usr/bin/env bash
# ==============================================================================
# setup_dataset.sh - Generates test dataset for IT305 Socket Programming Project
# Creates all 10 official topics with synthetic image/binary files for testing.
# Also provides instructions for downloading the full 800MB Kaggle dataset.
# ==============================================================================
set -euo pipefail

TARGET_DIR="${1:-./dataset}"
echo "Setting up topic dataset in: $TARGET_DIR"
mkdir -p "$TARGET_DIR"

TOPICS=(
  "cane"
  "cavallo"
  "elefante"
  "Farfalla"
  "Gallina"
  "Gatto"
  "Mucca"
  "Pecora"
  "Ragno"
  "Scoiattolo"
)

# Also create lowercase aliases if filesystem is case-sensitive
for topic in "${TOPICS[@]}"; do
  TOPIC_DIR="$TARGET_DIR/$topic"
  mkdir -p "$TOPIC_DIR"
  
  # Create a mix of small text, small image mock, and binary files
  echo "Metadata for topic $topic" > "$TOPIC_DIR/info.txt"
  
  # 64 KB file (1 chunk)
  dd if=/dev/urandom of="$TOPIC_DIR/img_small_1.jpg" bs=65536 count=1 status=none
  # 256 KB file (4 chunks)
  dd if=/dev/urandom of="$TOPIC_DIR/img_med_2.jpg" bs=65536 count=4 status=none
  # 1 MB file (16 chunks)
  dd if=/dev/urandom of="$TOPIC_DIR/img_large_3.jpg" bs=65536 count=16 status=none

  # If topic has uppercase, create lowercase symlink/directory for flexible testing
  lower_topic=$(echo "$topic" | tr '[:upper:]' '[:lower:]')
  if [ "$lower_topic" != "$topic" ] && [ ! -e "$TARGET_DIR/$lower_topic" ]; then
    ln -s "$topic" "$TARGET_DIR/$lower_topic" 2>/dev/null || cp -r "$TOPIC_DIR" "$TARGET_DIR/$lower_topic"
  fi
done

echo "======================================================================"
echo "Dataset successfully generated at: $TARGET_DIR"
echo "Topics created:"
ls -1 "$TARGET_DIR"
echo "======================================================================"
echo ""
echo "To download the full official 800 MB animals10 dataset from Kaggle:"
echo "  1. pip install kaggle"
echo "  2. Place your kaggle.json in ~/.kaggle/"
echo "  3. Run: kaggle datasets download -d alessiocorrado99/animals10 --unzip -p $TARGET_DIR"
