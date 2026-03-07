#!/bin/bash
#
# EC2 setup script for M5 Audio Platform backend
# Run as: sudo bash setup.sh
#
# Prerequisites:
#   - Ubuntu 22.04+ EC2 instance
#   - Port 8888 open in the security group
#   - Repo cloned to /home/ubuntu/M5_Stick_Audio_Proto

set -e

APP_DIR="/home/ubuntu/M5_Stick_Audio_Proto/backend"
SERVICE_FILE="/etc/systemd/system/m5audio.service"

echo "=== M5 Audio Platform — EC2 Setup ==="

# 1. Install system dependencies
echo "[1/5] Installing system packages..."
apt-get update -qq
apt-get install -y -qq python3 python3-venv python3-pip git

# 2. Create virtual environment and install Python deps
echo "[2/5] Setting up Python environment..."
cd "$APP_DIR"
if [ ! -d "venv" ]; then
    python3 -m venv venv
fi
source venv/bin/activate
pip install --upgrade pip -q
pip install -r requirements.txt -q
pip install python-dotenv -q
deactivate

# 3. Create .env if it doesn't exist
if [ ! -f "$APP_DIR/.env" ]; then
    echo "[3/5] Creating .env file (fill in your API keys)..."
    cat > "$APP_DIR/.env" <<'ENVEOF'
ELEVENLABS_API_KEY=
OPENAI_API_KEY=
JWT_SECRET=
ENVEOF
    chown ubuntu:ubuntu "$APP_DIR/.env"
    chmod 600 "$APP_DIR/.env"
    echo "  >> Edit /home/ubuntu/M5_Stick_Audio_Proto/backend/.env with your API keys"
else
    echo "[3/5] .env already exists, skipping..."
fi

# 4. Install systemd service
echo "[4/5] Installing systemd service..."
cp /home/ubuntu/M5_Stick_Audio_Proto/deploy/m5audio.service "$SERVICE_FILE"
systemctl daemon-reload
systemctl enable m5audio
systemctl restart m5audio

# 5. Verify
echo "[5/5] Checking service status..."
sleep 2
systemctl status m5audio --no-pager || true

echo ""
echo "=== Setup Complete ==="
echo "Backend running on http://$(curl -s http://169.254.169.254/latest/meta-data/public-ipv4 2>/dev/null || echo '<your-ip>'):8888"
echo ""
echo "Next steps:"
echo "  1. Edit .env with API keys:  nano $APP_DIR/.env"
echo "  2. Restart after editing:    sudo systemctl restart m5audio"
echo "  3. View logs:                journalctl -u m5audio -f"
echo "  4. Make sure port 8888 is open in your AWS Security Group"
