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

  const vectorStoreId = getVectorStoreId();
  const useFileSearch = Boolean(vectorStoreId);
  const payload = {
    model: chatConfig.openaiModel,
    instructions: SYSTEM_INSTRUCTIONS,
    input: buildInput(message, history, useFileSearch),
    max_output_tokens: normalizePositiveInt(chatConfig.openaiMaxOutputTokens),
  };
  applyResponseTuning(payload);
  if (useFileSearch) {
    payload.tools = [
      {
        type: 'file_search',
        vector_store_ids: [vectorStoreId],
        max_num_results: normalizePositiveInt(chatConfig.openaiFileSearchMaxResults),
      },
    ];
    payload.include = ['file_search_call.results'];
  }

  const response = await fetchOpenAiResponse(payload);
  const replyText = extractOutputText(response);
  if (useFileSearch && isDocumentFallbackText(replyText)) {
    const retryResponse = await fetchOpenAiResponse(buildRagRetryPayload(payload, message));
    const retryText = extractOutputText(retryResponse);
    if (!isDocumentFallbackText(retryText)) {
      return retryText;
    }
  }
  if (
    useFileSearch &&
    isDocumentFallbackText(replyText) &&
    !hasFileSearchResults(response) &&
    await hasIncompleteVectorStoreFiles(vectorStoreId)
  ) {
    return 'Tài liệu đang được hệ thống xử lý để tìm kiếm. Bạn thử hỏi lại sau ít phút nhé.';
  }
  return replyText;
}

function buildRagRetryPayload(payload, message) {
  return {
    ...payload,
    input: [
      payload.input,
      '',
      'Lượt tìm kiếm bổ sung:',
      'Câu trả lời trước đó không tìm thấy căn cứ. Hãy search lại toàn bộ tài liệu với nhiều biến thể từ khóa, từ đồng nghĩa, tên file, chủ đề tài liệu và cách viết không dấu/có dấu.',
      `Truy vấn mở rộng: ${buildRagRetryQuery(message)}`,
      'Nếu tìm thấy đoạn liên quan, hãy trả lời dựa trên đoạn đó. Nếu vẫn không có căn cứ, mới dùng câu fallback.',
    ].join('\n'),
    max_output_tokens: Math.max(normalizePositiveInt(payload.max_output_tokens, 800), 1200),
  };
}

function buildRagRetryQuery(message) {
  return [
    message,
    normalizeRetrievalQuery(message),
    'nội quy kỷ luật phòng thi giám thị phúc khảo vi phạm cảnh cáo đình chỉ thôi học',
    'kỳ thi sao chép gian lận điểm thi thời gian phúc khảo hình thức xử lý gian lận',
    'thủ tục đơn từ hành chính nộp đơn bảo lưu chuyển ngành nghỉ học tạm thời học phí Phòng Tài vụ Phòng Tuyển sinh',
    'định hướng sinh viên mới kỳ 1 ngành khoa chương trình đào tạo thư viện LMS email sinh viên',
    'quy chế đào tạo khóa luận tốt nghiệp học lại tích lũy tín chỉ OJT nghĩa vụ tài chính giáo dục quốc phòng giáo dục thể chất điểm trung bình',
  ].join(' | ');
}

async function fetchOpenAiResponse(payload) {
  const response = await postOpenAiResponse(payload);
  if (!isMaxOutputIncomplete(response)) {
    return response;
  }

  const retryPayload = {
    ...payload,
    max_output_tokens: Math.max(
      normalizePositiveInt(payload.max_output_tokens, 800),
      normalizePositiveInt(chatConfig.openaiRetryMaxOutputTokens, 1400)
    ),
  };
  return postOpenAiResponse(retryPayload);
}

async function postOpenAiResponse(payload) {
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

  return response.json();
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
  return synthesizeWithViettel(text, options);
}

export async function synthesizeWithFpt(text, options = {}) {
  return synthesizeWithViettel(text, options);
}

export async function synthesizeWithViettel(text, options = {}) {
  if (!chatConfig.viettelTtsToken) {
    throw new Error('VIETTEL_TTS_TOKEN is required');
  }

  const response = await fetch(chatConfig.viettelTtsUrl, {
    method: 'POST',
    headers: {
      accept: '*/*',
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({
      text,
      voice: options.voice || chatConfig.viettelTtsVoice,
      speed: normalizeViettelSpeed(options.speed ?? chatConfig.viettelTtsSpeed),
      tts_return_option: normalizeViettelReturnOption(options.tts_return_option ?? chatConfig.viettelTtsReturnOption),
      token: chatConfig.viettelTtsToken,
      without_filter: options.without_filter ?? chatConfig.viettelTtsWithoutFilter,
    }),
  });

  const contentType = response.headers.get('content-type') || '';
  const body = Buffer.from(await response.arrayBuffer());
  if (!response.ok) {
    throw new Error(`Viettel TTS failed: ${formatTtsError(body, contentType)}`);
  }
  if (contentType.startsWith('audio/')) {
    return body;
  }

  if (looksLikeWav(body) || looksLikeMp3(body)) {
    return body;
  }

  throw new Error(`Viettel TTS did not return audio: ${formatTtsError(body, contentType)}`);
}

function normalizeViettelSpeed(value) {
  const parsed = Number.parseFloat(value);
  if (!Number.isFinite(parsed)) {
    return 1.0;
  }
  return Math.min(1.2, Math.max(0.8, parsed));
}

function normalizeViettelReturnOption(value) {
  const parsed = Number.parseInt(value, 10);
  return parsed === 3 ? 3 : 2;
}

function looksLikeWav(buffer) {
  return buffer.subarray(0, 4).toString() === 'RIFF' && buffer.subarray(8, 12).toString() === 'WAVE';
}

function looksLikeMp3(buffer) {
  return buffer.subarray(0, 3).toString() === 'ID3' || [0xff, 0xfb, 0xf3, 0xf2].includes(buffer[0]);
}

function formatTtsError(buffer, contentType) {
  const text = buffer.toString('utf8').trim();
  if (!text) {
    return `empty response content-type=${contentType || 'unknown'}`;
  }
  try {
    const payload = JSON.parse(text);
    return payload.vi_message || payload.en_message || JSON.stringify(payload);
  } catch {
    return text;
  }
}

export async function uploadOpenAiDocument(file) {
  if (!chatConfig.openaiApiKey) {
    throw new Error('OPENAI_API_KEY is required');
  }

  const normalizedFile = await normalizeDocumentUpload(file);
  const vectorStoreId = await getOrCreateVectorStoreId();
  const fileForm = new FormData();
  fileForm.set('purpose', 'assistants');
  fileForm.set(
    'file',
    new Blob([normalizedFile.buffer], { type: normalizedFile.mimetype || 'application/octet-stream' }),
    normalizedFile.uploadName || normalizedFile.originalname
  );

  const fileResponse = await fetch('https://api.openai.com/v1/files', {
    method: 'POST',
    headers: { Authorization: `Bearer ${chatConfig.openaiApiKey}` },
    body: fileForm,
  });
  if (!fileResponse.ok) {
    throw new Error(`OpenAI file upload failed: ${await fileResponse.text()}`);
  }
  const filePayload = await fileResponse.json();

  const attachResponse = await fetchWithRetry(`https://api.openai.com/v1/vector_stores/${vectorStoreId}/files`, {
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
  const vectorFilePayload = await waitForVectorStoreFile(vectorStoreId, (await attachResponse.json()).id);

  return {
    filename: normalizedFile.displayName || normalizedFile.originalname || 'document',
    content_type: normalizedFile.displayMimeType || normalizedFile.mimetype || 'application/octet-stream',
    size: normalizedFile.displaySize || normalizedFile.size,
    file_id: filePayload.id,
    vector_store_id: vectorStoreId,
    vector_store_file_id: vectorFilePayload.id,
    status: vectorFilePayload.status || 'in_progress',
  };
}

export async function deleteOpenAiDocument(vectorStoreId, fileId) {
  if (!vectorStoreId || !fileId) {
    console.warn('OpenAI document delete skipped because vectorStoreId or fileId is missing');
    return;
  }

  const headers = { Authorization: `Bearer ${chatConfig.openaiApiKey}` };
  const vectorResponse = await fetch(`https://api.openai.com/v1/vector_stores/${vectorStoreId}/files/${fileId}`, {
    method: 'DELETE',
    headers,
  });
  if (!vectorResponse.ok) {
    const text = await vectorResponse.text();
    if (!isIgnorableOpenAiDeleteError(vectorResponse.status, text)) {
      throw new Error(`OpenAI vector delete failed: ${text}`);
    }
    console.warn(`OpenAI vector delete ignored status=${vectorResponse.status}: ${text}`);
  }

  const fileResponse = await fetch(`https://api.openai.com/v1/files/${fileId}`, {
    method: 'DELETE',
    headers,
  });
  if (!fileResponse.ok) {
    const text = await fileResponse.text();
    if (!isIgnorableOpenAiDeleteError(fileResponse.status, text)) {
      throw new Error(`OpenAI file delete failed: ${text}`);
    }
    console.warn(`OpenAI file delete ignored status=${fileResponse.status}: ${text}`);
  }
}

export async function listOpenAiVectorStoreFiles(vectorStoreId = getVectorStoreId()) {
  if (!vectorStoreId || !chatConfig.openaiApiKey) {
    return [];
  }

  const files = [];
  let after = '';
  do {
    const url = new URL(`https://api.openai.com/v1/vector_stores/${vectorStoreId}/files`);
    url.searchParams.set('limit', '100');
    if (after) {
      url.searchParams.set('after', after);
    }
    const response = await fetch(url, {
      headers: { Authorization: `Bearer ${chatConfig.openaiApiKey}` },
    });
    if (!response.ok) {
      throw new Error(`OpenAI vector file list failed: ${await response.text()}`);
    }
    const payload = await response.json();
    files.push(...(payload.data || []));
    after = payload.has_more ? payload.last_id || '' : '';
  } while (after);

  return files;
}

async function hasIncompleteVectorStoreFiles(vectorStoreId) {
  try {
    const files = await listOpenAiVectorStoreFiles(vectorStoreId);
    return files.some((file) => ['in_progress', 'queued'].includes(file.status));
  } catch (error) {
    console.warn(`OpenAI vector file status check skipped: ${error.message}`);
    return false;
  }
}

function buildInput(message, history, useFileSearch = false) {
  const lines = ['Lịch sử gần đây:'];
  const recentHistory = history
    .filter((item) => !isDocumentFallbackReply(item))
    .slice(-normalizePositiveInt(chatConfig.openaiRecentHistoryLimit, 6));
  for (const item of recentHistory) {
    lines.push(`${item.role || 'user'}: ${item.content || ''}`);
  }
  const normalizedQuery = useFileSearch ? normalizeRetrievalQuery(message) : message;
  if (normalizedQuery !== message) {
    lines.push(`user_original: ${message}`);
    lines.push(`user: ${normalizedQuery}`);
  } else {
    lines.push(`user: ${message}`);
  }
  if (useFileSearch) {
    lines.push('Yêu cầu RAG: nếu câu hỏi có thể nằm trong tài liệu đã upload, hãy dùng file_search trước khi trả lời.');
  }
  return lines.join('\n');
}

function normalizeRetrievalQuery(message) {
  let normalized = message
    .replace(/\bcourse\s*ra\b/giu, 'Coursera')
    .replace(/\bcourse\s*-?\s*ra\b/giu, 'Coursera')
    .trim();
  if (/coursera/iu.test(normalized) && /bắt đầu|bat dau|mã gì|ma gi/iu.test(normalized)) {
    normalized = `${normalized}. Tìm thông tin liên quan: môn học Coursera có chữ c sau cùng ở mã học phần.`;
  }
  return normalized;
}

function isDocumentFallbackReply(item) {
  return (
    item?.role === 'assistant' &&
    typeof item.content === 'string' &&
    item.content.toLowerCase().includes('mình chưa có thông tin này trong tài liệu quy chế')
  );
}

function isDocumentFallbackText(text) {
  return typeof text === 'string' && text.toLowerCase().includes('mình chưa có thông tin này trong tài liệu quy chế');
}

function hasFileSearchResults(payload) {
  return (payload.output || []).some((item) => item.type === 'file_search_call' && (item.results || []).length > 0);
}

async function waitForVectorStoreFile(vectorStoreId, vectorStoreFileId) {
  let lastPayload = null;
  for (let attempt = 0; attempt < normalizePositiveInt(chatConfig.openaiVectorFilePollAttempts, 45); attempt += 1) {
    const response = await fetch(`https://api.openai.com/v1/vector_stores/${vectorStoreId}/files/${vectorStoreFileId}`, {
      headers: { Authorization: `Bearer ${chatConfig.openaiApiKey}` },
    });
    if (!response.ok) {
      throw new Error(`OpenAI vector file status failed: ${await response.text()}`);
    }
    const payload = await response.json();
    lastPayload = payload;
    if (payload.status === 'completed') {
      return payload;
    }
    if (['failed', 'cancelled'].includes(payload.status)) {
      throw new Error(`OpenAI vector file indexing ${payload.status}: ${JSON.stringify(payload.last_error || payload)}`);
    }
    await delay(normalizePositiveInt(chatConfig.openaiVectorFilePollIntervalMs, 2000));
  }
  throw new Error(`OpenAI vector file indexing timed out: ${JSON.stringify(lastPayload)}`);
}

async function normalizeDocumentUpload(file) {
  const originalname = decodeMultipartFilename(file.originalname || 'document');
  const converted = await convertLegacyPowerPointIfNeeded(file.buffer, originalname);
  const buffer = normalizeTextFileBuffer(converted.buffer, converted.originalname, converted.mimetype || file.mimetype || '');
  return {
    ...file,
    originalname: converted.originalname,
    displayName: originalname,
    displayMimeType: file.mimetype,
    displaySize: file.size,
    uploadName: converted.originalname,
    buffer,
    size: buffer.length,
    mimetype: converted.mimetype || file.mimetype,
  };
}

async function convertLegacyPowerPointIfNeeded(buffer, originalname) {
  if (path.extname(originalname).toLowerCase() !== '.ppt') {
    return { buffer, originalname, mimetype: '' };
  }

  const converted = await convertOfficeDocumentToPdf(buffer, originalname);
  if (!converted) {
    const error = new Error('Không thể tách nội dung file .ppt cũ. Vui lòng upload .pptx hoặc .pdf, hoặc cài LibreOffice trên server để tự convert .ppt.');
    error.statusCode = 400;
    throw error;
  }
  return converted;
}

async function convertOfficeDocumentToPdf(buffer, originalname) {
  const tempDir = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'unimate_doc_'));
  const inputPath = path.join(tempDir, path.basename(originalname));
  const outputPath = path.join(tempDir, `${path.parse(originalname).name}.pdf`);
  try {
    await fs.promises.writeFile(inputPath, buffer);
    const command = await findLibreOfficeCommand();
    if (!command) {
      return null;
    }
    await runCommand(command, ['--headless', '--convert-to', 'pdf', '--outdir', tempDir, inputPath]);
    const pdfBuffer = await fs.promises.readFile(outputPath);
    return {
      buffer: pdfBuffer,
      originalname: `${path.parse(originalname).name}.pdf`,
      mimetype: 'application/pdf',
    };
  } catch (error) {
    console.warn(`PPT->PDF conversion failed: ${error.message}`);
    return null;
  } finally {
    await fs.promises.rm(tempDir, { recursive: true, force: true });
  }
}

async function findLibreOfficeCommand() {
  for (const command of [process.env.LIBREOFFICE_PATH, 'soffice', 'libreoffice'].filter(Boolean)) {
    try {
      await runCommand(command, ['--version']);
      return command;
    } catch {
      // Try the next common command name.
    }
  }
  return '';
}

function runCommand(command, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { windowsHide: true });
    let stderr = '';
    child.stderr.on('data', (chunk) => {
      stderr += chunk.toString();
    });
    child.on('error', reject);
    child.on('close', (code) => {
      if (code === 0) {
        resolve();
      } else {
        reject(new Error(stderr.trim() || `${command} exited with code ${code}`));
      }
    });
  });
}

function decodeMultipartFilename(filename) {
  if (!filename) {
    return 'document';
  }
  try {
    return Buffer.from(filename, 'latin1').toString('utf8');
  } catch {
    return filename;
  }
}

function normalizeTextFileBuffer(buffer, filename, mimetype) {
  if (!isTextLikeDocument(filename, mimetype)) {
    return buffer;
  }

  const encoding = detectTextEncoding(buffer);
  const decoded = decodeBuffer(buffer, encoding);
  return Buffer.from(stripBom(decoded), 'utf8');
}

function isTextLikeDocument(filename, mimetype) {
  const lowerName = filename.toLowerCase();
  const lowerType = mimetype.toLowerCase();
  return (
    lowerType.startsWith('text/') ||
    lowerType.includes('json') ||
    lowerType.includes('xml') ||
    ['.txt', '.md', '.csv', '.json', '.xml', '.html', '.htm'].includes(path.extname(lowerName))
  );
}

function detectTextEncoding(buffer) {
  if (buffer.length >= 3 && buffer[0] === 0xef && buffer[1] === 0xbb && buffer[2] === 0xbf) {
    return 'utf-8';
  }
  if (buffer.length >= 2 && buffer[0] === 0xff && buffer[1] === 0xfe) {
    return 'utf-16le';
  }
  if (looksLikeUtf16LeWithoutBom(buffer)) {
    return 'utf-16le';
  }
  if (buffer.toString('utf8').includes('\uFFFD')) {
    return 'windows-1258';
  }
  return 'utf-8';
}

function looksLikeUtf16LeWithoutBom(buffer) {
  const sample = buffer.subarray(0, Math.min(buffer.length, 64));
  let zeroBytes = 0;
  for (let index = 1; index < sample.length; index += 2) {
    if (sample[index] === 0) {
      zeroBytes += 1;
    }
  }
  return zeroBytes >= Math.floor(sample.length / 4);
}

function decodeBuffer(buffer, encoding) {
  try {
    return new TextDecoder(encoding, { fatal: false }).decode(buffer);
  } catch {
    return buffer.toString('utf8');
  }
}

function stripBom(text) {
  return text.replace(/^\uFEFF/, '');
}

function normalizePositiveInt(value, fallback = undefined) {
  const parsed = Number.parseInt(value, 10);
  if (Number.isInteger(parsed) && parsed > 0) {
    return parsed;
  }
  return fallback;
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

function applyResponseTuning(payload) {
  if (supportsReasoningOptions(payload.model)) {
    payload.reasoning = { effort: chatConfig.openaiReasoningEffort || 'low' };
    payload.text = { verbosity: chatConfig.openaiTextVerbosity || 'low' };
  }
}

function supportsReasoningOptions(model = '') {
  return /^gpt-5/i.test(model);
}

function isMaxOutputIncomplete(payload) {
  return payload?.status === 'incomplete' && payload?.incomplete_details?.reason === 'max_output_tokens';
}

function parseOpenAiError(text) {
  try {
    const payload = JSON.parse(text);
    return payload.error?.message || text;
  } catch {
    return text;
  }
}

function isIgnorableOpenAiDeleteError(status, text) {
  if (status === 404) {
    return true;
  }
  if (status !== 400) {
    return false;
  }
  try {
    const payload = JSON.parse(text);
    return payload.error?.code === 'invalid_value';
  } catch {
    return text.includes('invalid_value');
  }
}

async function fetchWithRetry(url, options) {
  let response;
  for (let attempt = 0; attempt < 5; attempt += 1) {
    response = await fetch(url, options);
    if (![429, 500, 502, 503, 504].includes(response.status)) {
      return response;
    }
    await delay(1000 * (attempt + 1));
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
