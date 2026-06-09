import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';
import { SYSTEM_INSTRUCTIONS, chatConfig } from './config.js';

export async function generateReply(message, sessionId, history = []) {
  if (chatConfig.llmProvider === 'mock') {
    return `Toi da nhan: ${message}. Day la cau tra loi mau cho session ${sessionId.slice(0, 8)}.`;
  }
  if (chatConfig.llmProvider !== 'openai') {
    throw new Error(`Unsupported LLM_PROVIDER: ${chatConfig.llmProvider}`);
  }
  if (!chatConfig.openaiApiKey) {
    throw new Error('OPENAI_API_KEY is required');
  }

  const payload = {
    model: chatConfig.openaiModel,
    instructions: SYSTEM_INSTRUCTIONS,
    input: buildInput(message, history),
  };
  const vectorStoreId = getVectorStoreId();
  if (vectorStoreId) {
    payload.tools = [{ type: 'file_search', vector_store_ids: [vectorStoreId] }];
  }

  const response = await fetchWithRetry('https://api.openai.com/v1/responses', {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${chatConfig.openaiApiKey}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify(payload),
  });

  if (!response.ok) {
    const text = await response.text();
    const error = new Error(parseOpenAiError(text));
    error.statusCode = response.status;
    throw error;
  }

  return extractOutputText(await response.json());
}

export async function transcribe(audioBuffer, filename, contentType) {
  if (chatConfig.sttProvider === 'mock') {
    return 'Day la transcript mau tu audio.';
  }
  return transcribeWithFpt(audioBuffer, filename, contentType);
}

export async function transcribeWithFpt(audioBuffer, filename, contentType) {
  const payload = await transcribeRawWithFpt(audioBuffer, filename, contentType);
  const transcript = extractTranscript(payload);
  if (!transcript) {
    throw new Error(`FPT ASR response did not include transcript: ${JSON.stringify(payload)}`);
  }
  return transcript;
}

export async function transcribeRawWithFpt(audioBuffer, filename, contentType) {
  if (!chatConfig.fptAsrApiKey) {
    throw new Error('FPT_ASR_API_KEY is required');
  }

  const candidates = await buildFptAsrCandidates(audioBuffer, filename, contentType);
  let lastPayload = null;
  let lastStatus = 0;

  for (const candidate of candidates) {
    const headers = {
      'api-key': chatConfig.fptAsrApiKey,
      api_key: chatConfig.fptAsrApiKey,
    };
    if (candidate.contentType) {
      headers['Content-Type'] = candidate.contentType;
    }

    const response = await fetch(chatConfig.fptAsrUrl, {
      method: 'POST',
      headers,
      body: candidate.buffer,
    });
    const payload = await readJsonResponse(response);
    lastPayload = payload;
    lastStatus = response.status;

    if (response.ok && isFptAsrPayloadOk(payload)) {
      return payload;
    }

    console.warn(
      `FPT ASR attempt failed filename=${candidate.filename} contentType=${candidate.contentType || 'none'} http=${response.status}: ${JSON.stringify(payload)}`
    );
  }

  const error = new Error(`FPT ASR failed: ${JSON.stringify(lastPayload)}`);
  error.statusCode = lastStatus || 502;
  throw error;
}

async function buildFptAsrCandidates(audioBuffer, filename, contentType) {
  const candidates = [];
  const looksLikeWav = isWavAudio(audioBuffer, filename, contentType);

  if (looksLikeWav) {
    const m4aBuffer = await transcodeWavToM4a(audioBuffer);
    if (m4aBuffer) {
      candidates.push({
        buffer: m4aBuffer,
        filename: replaceExt(filename || 'recording.wav', '.m4a'),
        contentType: 'audio/x-m4a',
      });
    }
  }

  candidates.push({ buffer: audioBuffer, filename: filename || 'recording', contentType: contentType || '' });
  if (looksLikeWav) {
    candidates.push({ buffer: audioBuffer, filename: filename || 'recording.wav', contentType: '' });
    candidates.push({ buffer: audioBuffer, filename: filename || 'recording.wav', contentType: 'application/octet-stream' });
  }

  return dedupeAsrCandidates(candidates);
}

function isFptAsrPayloadOk(payload) {
  if (!payload || typeof payload !== 'object') {
    return false;
  }
  if (payload.status !== undefined && payload.status !== 0 && payload.status !== '0') {
    return false;
  }
  return Boolean(extractTranscript(payload));
}

function isWavAudio(audioBuffer, filename = '', contentType = '') {
  const lowerName = filename.toLowerCase();
  const lowerType = contentType.toLowerCase();
  return (
    lowerType.includes('wav') ||
    lowerName.endsWith('.wav') ||
    (audioBuffer.subarray(0, 4).toString() === 'RIFF' && audioBuffer.subarray(8, 12).toString() === 'WAVE')
  );
}

function replaceExt(filename, ext) {
  const parsed = path.parse(filename || 'recording');
  return `${parsed.name || 'recording'}${ext}`;
}

function dedupeAsrCandidates(candidates) {
  const seen = new Set();
  return candidates.filter((candidate) => {
    const key = `${candidate.filename}|${candidate.contentType}`;
    if (seen.has(key)) {
      return false;
    }
    seen.add(key);
    return true;
  });
}

async function transcodeWavToM4a(audioBuffer) {
  const tempBase = path.join(os.tmpdir(), `unimate_asr_${randomUUID()}`);
  const inputPath = `${tempBase}.wav`;
  const outputPath = `${tempBase}.m4a`;

  try {
    await fs.promises.writeFile(inputPath, audioBuffer);
    await runFfmpeg([
      '-hide_banner',
      '-loglevel',
      'error',
      '-y',
      '-i',
      inputPath,
      '-vn',
      '-ac',
      '1',
      '-ar',
      '16000',
      '-c:a',
      'aac',
      '-b:a',
      '64k',
      outputPath,
    ]);
    return await fs.promises.readFile(outputPath);
  } catch (error) {
    console.warn(`WAV->M4A transcode skipped: ${error.message}`);
    return null;
  } finally {
    await Promise.allSettled([fs.promises.unlink(inputPath), fs.promises.unlink(outputPath)]);
  }
}

function runFfmpeg(args) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.env.FFMPEG_PATH || 'ffmpeg', args, { windowsHide: true });
    let stderr = '';

    child.stderr.on('data', (chunk) => {
      stderr += chunk.toString();
    });
    child.on('error', reject);
    child.on('close', (code) => {
      if (code === 0) {
        resolve();
      } else {
        reject(new Error(stderr.trim() || `ffmpeg exited with code ${code}`));
      }
    });
  });
}

export async function synthesize(text, options = {}) {
  if (!text.trim()) {
    return null;
  }
  if (chatConfig.ttsProvider === 'mock') {
    return createSilentWav();
  }
  return synthesizeWithFpt(text, options);
}

export async function synthesizeWithFpt(text, options = {}) {
  if (!chatConfig.fptTtsApiKey) {
    throw new Error('FPT_TTS_API_KEY is required');
  }

  const response = await fetch(chatConfig.fptTtsUrl, {
    method: 'POST',
    headers: {
      'api-key': chatConfig.fptTtsApiKey,
      voice: options.voice || chatConfig.fptTtsVoice,
      speed: options.speed ?? chatConfig.fptTtsSpeed,
      'Content-Type': 'text/plain; charset=utf-8',
    },
    body: Buffer.from(text, 'utf8'),
  });

  const contentType = response.headers.get('content-type') || '';
  const body = Buffer.from(await response.arrayBuffer());
  if (!response.ok) {
    throw new Error(`FPT TTS failed: ${body.toString('utf8')}`);
  }
  if (contentType.startsWith('audio/')) {
    return body;
  }

  const payload = JSON.parse(body.toString('utf8'));
  const audioUrl = extractAudioUrl(payload);
  if (!audioUrl) {
    throw new Error(`FPT TTS response did not include audio URL: ${JSON.stringify(payload)}`);
  }
  return pollAudio(audioUrl);
}

export async function uploadOpenAiDocument(file) {
  if (!chatConfig.openaiApiKey) {
    throw new Error('OPENAI_API_KEY is required');
  }

  const vectorStoreId = await getOrCreateVectorStoreId();
  const fileForm = new FormData();
  fileForm.set('purpose', 'assistants');
  fileForm.set('file', new Blob([file.buffer], { type: file.mimetype || 'application/octet-stream' }), file.originalname);

  const fileResponse = await fetch('https://api.openai.com/v1/files', {
    method: 'POST',
    headers: { Authorization: `Bearer ${chatConfig.openaiApiKey}` },
    body: fileForm,
  });
  if (!fileResponse.ok) {
    throw new Error(`OpenAI file upload failed: ${await fileResponse.text()}`);
  }
  const filePayload = await fileResponse.json();

  const attachResponse = await fetch(`https://api.openai.com/v1/vector_stores/${vectorStoreId}/files`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${chatConfig.openaiApiKey}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({ file_id: filePayload.id }),
  });
  if (!attachResponse.ok) {
    throw new Error(`OpenAI vector attach failed: ${await attachResponse.text()}`);
  }
  const vectorFilePayload = await attachResponse.json();

  return {
    filename: file.originalname || 'document',
    content_type: file.mimetype || 'application/octet-stream',
    size: file.size,
    file_id: filePayload.id,
    vector_store_id: vectorStoreId,
    vector_store_file_id: vectorFilePayload.id,
    status: vectorFilePayload.status || 'in_progress',
  };
}

export async function deleteOpenAiDocument(vectorStoreId, fileId) {
  const headers = { Authorization: `Bearer ${chatConfig.openaiApiKey}` };
  const vectorResponse = await fetch(`https://api.openai.com/v1/vector_stores/${vectorStoreId}/files/${fileId}`, {
    method: 'DELETE',
    headers,
  });
  if (!vectorResponse.ok && vectorResponse.status !== 404) {
    throw new Error(`OpenAI vector delete failed: ${await vectorResponse.text()}`);
  }

  const fileResponse = await fetch(`https://api.openai.com/v1/files/${fileId}`, {
    method: 'DELETE',
    headers,
  });
  if (!fileResponse.ok && fileResponse.status !== 404) {
    throw new Error(`OpenAI file delete failed: ${await fileResponse.text()}`);
  }
}

function buildInput(message, history) {
  const lines = ['Lich su gan day:'];
  for (const item of history.slice(-10)) {
    lines.push(`${item.role || 'user'}: ${item.content || ''}`);
  }
  lines.push(`user: ${message}`);
  return lines.join('\n');
}

function getVectorStoreId() {
  if (chatConfig.openaiVectorStoreId) {
    return chatConfig.openaiVectorStoreId;
  }
  if (fs.existsSync(chatConfig.openaiVectorStoreIdFile)) {
    return fs.readFileSync(chatConfig.openaiVectorStoreIdFile, 'utf8').trim();
  }
  return '';
}

async function getOrCreateVectorStoreId() {
  const existing = getVectorStoreId();
  if (existing) {
    return existing;
  }

  const response = await fetch('https://api.openai.com/v1/vector_stores', {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${chatConfig.openaiApiKey}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({ name: chatConfig.openaiVectorStoreName }),
  });
  if (!response.ok) {
    throw new Error(`OpenAI vector store create failed: ${await response.text()}`);
  }
  const vectorStoreId = (await response.json()).id;
  fs.mkdirSync(path.dirname(chatConfig.openaiVectorStoreIdFile), { recursive: true });
  fs.writeFileSync(chatConfig.openaiVectorStoreIdFile, vectorStoreId, 'utf8');
  return vectorStoreId;
}

function extractOutputText(payload) {
  if (typeof payload.output_text === 'string' && payload.output_text.trim()) {
    return payload.output_text.trim();
  }

  const parts = [];
  for (const output of payload.output || []) {
    for (const content of output.content || []) {
      if (typeof content.text === 'string' && content.text.trim()) {
        parts.push(content.text.trim());
      }
    }
  }
  if (parts.length > 0) {
    return parts.join('\n');
  }
  throw new Error(`OpenAI response did not include output text: ${JSON.stringify(payload)}`);
}

function parseOpenAiError(text) {
  try {
    const payload = JSON.parse(text);
    return payload.error?.message || text;
  } catch {
    return text;
  }
}

async function fetchWithRetry(url, options) {
  let response;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    response = await fetch(url, options);
    if (response.status !== 429) {
      return response;
    }
    await delay(1000 + attempt * 1000);
  }
  return response;
}

async function readJsonResponse(response) {
  const text = await response.text();
  try {
    return text ? JSON.parse(text) : {};
  } catch {
    return { raw: text };
  }
}

export function extractTranscript(payload) {
  for (const key of ['text', 'transcript', 'utterance', 'result']) {
    if (typeof payload[key] === 'string' && payload[key].trim()) {
      return payload[key].trim();
    }
  }
  if (Array.isArray(payload.hypotheses)) {
    for (const item of payload.hypotheses) {
      const transcript = extractTranscript(item || {});
      if (transcript) {
        return transcript;
      }
    }
  }
  if (payload.data && typeof payload.data === 'object') {
    return extractTranscript(payload.data);
  }
  return '';
}

function extractAudioUrl(payload) {
  if (payload.error !== undefined && payload.error !== 0 && payload.error !== '0') {
    throw new Error(`FPT TTS returned error: ${JSON.stringify(payload)}`);
  }
  for (const key of ['async', 'audio_url', 'url', 'link']) {
    if (typeof payload[key] === 'string' && payload[key].trim()) {
      return payload[key].trim();
    }
  }
  if (typeof payload.data === 'string' && payload.data.trim()) {
    return payload.data.trim();
  }
  if (payload.data && typeof payload.data === 'object') {
    return extractAudioUrl(payload.data);
  }
  return '';
}

async function pollAudio(audioUrl) {
  let lastError;
  for (let attempt = 0; attempt < chatConfig.fptTtsPollAttempts; attempt += 1) {
    try {
      const response = await fetch(audioUrl);
      if (response.ok) {
        const body = Buffer.from(await response.arrayBuffer());
        if (body.length > 0) {
          return body;
        }
      }
      if (![202, 404].includes(response.status)) {
        throw new Error(`Audio download failed with status ${response.status}`);
      }
    } catch (error) {
      lastError = error;
    }
    await delay(chatConfig.fptTtsPollIntervalMs);
  }
  throw lastError || new Error(`FPT TTS audio was not ready: ${audioUrl}`);
}

function createSilentWav() {
  const sampleRate = 16000;
  const samples = sampleRate;
  const dataSize = samples * 2;
  const buffer = Buffer.alloc(44 + dataSize);
  buffer.write('RIFF', 0);
  buffer.writeUInt32LE(36 + dataSize, 4);
  buffer.write('WAVEfmt ', 8);
  buffer.writeUInt32LE(16, 16);
  buffer.writeUInt16LE(1, 20);
  buffer.writeUInt16LE(1, 22);
  buffer.writeUInt32LE(sampleRate, 24);
  buffer.writeUInt32LE(sampleRate * 2, 28);
  buffer.writeUInt16LE(2, 32);
  buffer.writeUInt16LE(16, 34);
  buffer.write('data', 36);
  buffer.writeUInt32LE(dataSize, 40);
  return buffer;
}
