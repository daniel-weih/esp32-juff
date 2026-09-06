export class LogStore {
  constructor({ secrets = [], limit = 600, now = Date.now } = {}) {
    this.secrets = secrets.filter(value => typeof value === 'string' && value.length >= 8)
    this.limit = limit; this.now = now; this.sequence = 0; this.entries = []
  }
  redact(value) {
    let text = String(value).replace(/\x1b\[[0-9;]*[A-Za-z]/g, '')
    for (const secret of this.secrets) text = text.split(secret).join('[已隐藏]')
    return text
      .replace(/sk-[A-Za-z0-9_.-]{16,}|(?:gh[pousr]|github_pat)_[A-Za-z0-9_]{20,}/g, '[已隐藏]')
      .replace(/\bBearer\s+[A-Za-z0-9_.~+\/-]+/gi, 'Bearer [已隐藏]')
      .replace(/(["']?(?:authorization|cookie|password|api[_-]?key|token)["']?\s*[=:]\s*)(?:"[^"\n]*"|'[^'\n]*'|[^\s,;}]+)/gi, '$1[已隐藏]')
      .replace(/(https?:\/\/)[^\s/@:]+:[^\s/@]+@/g, '$1[已隐藏]@')
      .slice(0, 1200)
  }
  add(service, message, level = 'info') {
    // The management log is operational: omit microphone payloads and dialogue.
    if (/^(?:BLE )?MIC [A-Za-z0-9+/]+=*$/.test(message) || /^QWEN (?!EVENT )/.test(message)) return
    const text = this.redact(message)
    if (!text.trim()) return
    this.entries.push({ id: ++this.sequence, time: this.now(), service, level, message: text })
    if (this.entries.length > this.limit) this.entries.splice(0, this.entries.length - this.limit)
  }
  read({ service, after = 0, level } = {}) {
    return this.entries.filter(entry => entry.id > after && (!service || entry.service === service) && (!level || entry.level === level))
  }
}

export function readLines(stream, callback) {
  let buffer = ''
  stream?.setEncoding('utf8')
  stream?.on('data', chunk => {
    buffer += chunk
    let newline
    while ((newline = buffer.indexOf('\n')) >= 0) {
      callback(buffer.slice(0, newline).replace(/\r$/, '').slice(0, 8192))
      buffer = buffer.slice(newline + 1)
    }
    // A broken child must not grow the management process indefinitely.
    if (buffer.length > 16384) { callback(buffer.slice(0, 1200) + ' [已截断]'); buffer = '' }
  })
  stream?.on('end', () => { if (buffer) callback(buffer.slice(0, 1200)); buffer = '' })
}
