# xAI Voice Agent — Python
# pip install websockets  ·  export XAI_API_KEY="xai-..."

import asyncio, json, os
import websockets

async def main():
    headers = {"Authorization": f"Bearer {os.environ['XAI_API_KEY']}"}

    async with websockets.connect(
        "wss://api.x.ai/v1/realtime",
        extra_headers=headers
    ) as ws:
        await ws.send(json.dumps({
            "type": "session.update",
            "session": {
                "voice": "Eve",
                "instructions": AGENT_PROMPT,  # see Agent Prompt tab
                "turn_detection": {"type": "server_vad"},
                "tools": [{"type": "web_search"}, {"type": "x_search"}],
                "input_audio_transcription": {"model": "grok-2-audio"},
            },
        }))

        # Trigger agent to speak first
        await ws.send(json.dumps({"type": "response.create"}))

        async for raw in ws:
            event = json.loads(raw)
            etype = event["type"]

            if etype == "session.created":
                print(f"Session: {event['session']['id']}")

            elif etype == "input_audio_buffer.speech_started":
                await ws.send(json.dumps({"type": "response.cancel"}))

            elif etype == "response.output_audio.delta":
                # Base64 PCM audio → decode and play
                pcm = base64.b64decode(event["delta"])
                play_audio(pcm)

            elif etype == "response.output_audio_transcript.delta":
                print(event["delta"], end="", flush=True)

            elif etype == "response.done":
                print(f"\nTokens: {event.get('usage', {}).get('total_tokens')}")

asyncio.run(main())