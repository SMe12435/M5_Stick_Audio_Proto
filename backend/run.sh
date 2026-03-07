#!/bin/bash
# Run the M5 Audio Platform backend server
# Usage: ./run.sh
#
# Environment variables (optional):
#   DEEPGRAM_API_KEY  - For real-time transcription
#   OPENAI_API_KEY    - For LLM note extraction
#   JWT_SECRET        - For auth token signing (auto-generated if not set)

cd "$(dirname "$0")"

if [ ! -d "venv" ]; then
    echo "Creating virtual environment..."
    python3 -m venv venv
    source venv/bin/activate
    pip install -r requirements.txt
else
    source venv/bin/activate
fi

echo "Starting M5 Audio Platform backend on http://0.0.0.0:8000"
echo "Web portal: http://localhost:8000/"
echo ""
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
