#!/bin/bash

# Enable dmesg access for non-root users without password
# Run once: sudo /media/abu/CoderDrive/LeviathanOS/src/churro/enable-dmesg.sh

echo "Setting up dmesg access for Leviathan Dashboard..."

# Method 1: Allow dmesg via sudoers (no password)
echo "$USER ALL=(ALL) NOPASSWD: /bin/dmesg" | sudo tee /etc/sudoers.d/dmesg-nopasswd > /dev/null
sudo chmod 0440 /etc/sudoers.d/dmesg-nopasswd

echo "✅ dmesg now accessible without password!"
echo "Run: dmesg"
