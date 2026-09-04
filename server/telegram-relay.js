// Cloudflare Worker - Telegram Relay
// Deploy this as a free Cloudflare Worker to relay messages from your VPS to Telegram

addEventListener('fetch', event => {
  event.respondWith(handleRequest(event.request))
})

async function handleRequest(request) {
  if (request.method !== 'POST') {
    return new Response('POST only', { status: 405 })
  }

  // Auth check - simple shared secret
  const auth = request.headers.get('X-Relay-Key')
  if (auth !== RELAY_SECRET) {
    return new Response('Unauthorized', { status: 401 })
  }

  const body = await request.json()
  const { chatId, text } = body

  if (!chatId || !text) {
    return new Response(JSON.stringify({ error: 'chatId and text required' }), { status: 400 })
  }

  const tgUrl = `https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage`

  const tgResp = await fetch(tgUrl, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      chat_id: chatId,
      text: text,
      parse_mode: 'HTML'
    })
  })

  const result = await tgResp.json()
  return new Response(JSON.stringify(result), {
    headers: { 'Content-Type': 'application/json' }
  })
}
