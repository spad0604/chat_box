import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { spawn } from 'node:child_process';
import { chatConfig } from './config.js';

export function ensureStorageDirs() {
  fs.mkdirSync(chatConfig.audioStorageDir, { recursive: true });
  fs.mkdirSync(chatConfig.voiceUploadDir, { recursive: true });
}

export async function saveReplyAudio(audioBuffer) {
  ensureStorageDirs();
  const wavBuffer = await ensureWavBuffer(audioBuffer);
  const filename = `reply_${randomUUID().replaceAll('-', '')}.wav`;
  fs.writeFileSync(path.join(chatConfig.audioStorageDir, filename), wavBuffer);
  return filename;
}

export function resolveAudioPath(filename) {
  const safeName = path.basename(filename);
  const resolved = path.resolve(chatConfig.audioStorageDir, safeName);
  const root = path.resolve(chatConfig.audioStorageDir);

  if (!resolved.startsWith(root) || !fs.existsSync(resolved) || !fs.statSync(resolved).isFile()) {
    return null;
  }

  return resolved;
}

export function publicAudioUrl(filename) {
  return `${chatConfig.publicBaseUrl}/api/v1/audio/${filename}`;
}

export function publicEsp32AudioUrl(filename) {
  return `${chatConfig.publicBaseUrl}/api/v1/audio-esp32/${filename}`;
}

export async function resolveEsp32AudioPath(filename, sampleRate = 16000, bitsPerSample = 16) {
  const sourcePath = resolveAudioPath(filename);
  if (!sourcePath) {
    return null;
  }

  const rate = normalizeEsp32SampleRate(sampleRate);
  const bits = normalizeEsp32BitsPerSample(bitsPerSample);
  const parsed = path.parse(path.basename(sourcePath));
  const esp32Filename = `${parsed.name}.esp32-${rate}-${bits}bit.wav`;
  const esp32Path = path.resolve(chatConfig.audioStorageDir, esp32Filename);

  if (fs.existsSync(esp32Path)) {
    const sourceStat = fs.statSync(sourcePath);
    const esp32Stat = fs.statSync(esp32Path);
    if (esp32Stat.mtimeMs >= sourceStat.mtimeMs) {
      return esp32Path;
    }
  }

  try {
    await transcodeToEsp32Wav(sourcePath, esp32Path, rate, bits);
    return esp32Path;
  } catch (error) {
    console.warn(`ESP32 audio conversion failed for ${filename}: ${error.message}`);
    return sourcePath;
  }
}

export function detectAudioExtension(audioBuffer) {
  if (audioBuffer.subarray(0, 4).toString() === 'RIFF' && audioBuffer.subarray(8, 12).toString() === 'WAVE') {
    return '.wav';
  }
  if (
    audioBuffer.subarray(0, 3).toString() === 'ID3' ||
    [0xff, 0xfb, 0xf3, 0xf2].includes(audioBuffer[0])
  ) {
    return '.mp3';
  }
  return '.bin';
}

async function ensureWavBuffer(audioBuffer) {
  const tempBase = path.join(os.tmpdir(), `unimate_reply_${randomUUID()}`);
  const inputPath = `${tempBase}.audio`;
  const outputPath = `${tempBase}.wav`;

  try {
    fs.writeFileSync(inputPath, audioBuffer);
    await transcodeToEsp32Wav(inputPath, outputPath, 16000, 16);
    return fs.readFileSync(outputPath);
  } catch (error) {
    console.warn(`Reply audio WAV conversion failed, keeping original audio: ${error.message}`);
    return audioBuffer;
  } finally {
    await Promise.allSettled([fs.promises.unlink(inputPath), fs.promises.unlink(outputPath)]);
  }
}

function normalizeEsp32SampleRate(value) {
  const rate = Number.parseInt(value, 10);
  return [8000, 16000].includes(rate) ? rate : 16000;
}

function normalizeEsp32BitsPerSample(value) {
  const bits = Number.parseInt(value, 10);
  return [8, 16].includes(bits) ? bits : 16;
}

function transcodeToEsp32Wav(inputPath, outputPath, sampleRate = 16000, bitsPerSample = 16) {
  const rate = normalizeEsp32SampleRate(sampleRate);
  const bits = normalizeEsp32BitsPerSample(bitsPerSample);
  return runFfmpeg([
    '-hide_banner',
    '-loglevel',
    'error',
    '-y',
    '-i',
    inputPath,
    '-ac',
    '1',
    '-ar',
    String(rate),
    '-c:a',
    bits === 8 ? 'pcm_u8' : 'pcm_s16le',
    outputPath,
  ]);
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
