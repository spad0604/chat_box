import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { Router } from 'express';
import multer from 'multer';
import { query } from '../config/db.js';
import {
  appendMessage,
  getMessages,
  getSessions,
  topQuestions,
  unansweredQuestions,
  usageStats,
} from '../chat/chatStore.js';
import { chatConfig } from '../chat/config.js';
import {
  deleteOpenAiDocument,
  generateReply,
  listOpenAiVectorStoreFiles,
  synthesize,
  synthesizeWithViettel,
  transcribe,
  transcribeRawWithFpt,
  uploadOpenAiDocument,
  extractTranscript,
} from '../chat/providers.js';
import { ensureStorageDirs, publicAudioUrl, resolveAudioPath, saveReplyAudio } from '../chat/audioStorage.js';

const router = Router();
const upload = multer({ storage: multer.memoryStorage(), limits: { fileSize: 25 * 1024 * 1024 } });

router.get('/health', (_req, res) => {
  res.json({ status: 'ok' });
});

const uploadImage = multer({ storage: multer.memoryStorage(), limits: { fileSize: 512 * 1024 } });

// Helper to get all campus images sorted chronologically (alphabetically by timestamp name)
function getCampusImagesList() {
  const campusDir = path.resolve('storage', 'campus');
  if (!fs.existsSync(campusDir)) {
    return [];
  }
  return fs.readdirSync(campusDir)
    .filter(file => /\.(jpg|jpeg|png|webp|gif)$/i.test(file))
    .sort();
}

// POST: Upload a new campus image to the list
router.post('/campus-images', uploadImage.single('file'), (req, res, next) => {
  try {
    if (!req.file) {
      return res.status(400).json({ detail: 'file is required' });
    }
    if (!req.file.mimetype.startsWith('image/')) {
      return res.status(400).json({ detail: 'file must be an image' });
    }

    const campusDir = path.resolve('storage', 'campus');
    if (!fs.existsSync(campusDir)) {
      fs.mkdirSync(campusDir, { recursive: true });
    }

    // Generate chronological name
    const timestamp = Date.now();
    const rand = Math.random().toString(36).substring(2, 8);
    const ext = path.extname(req.file.originalname) || '.jpg';
    const filename = `image_${timestamp}_${rand}${ext}`;
    const imagePath = path.join(campusDir, filename);

    fs.writeFileSync(imagePath, req.file.buffer);

    return res.json({
      detail: 'Image uploaded successfully',
      filename,
      url: `${chatConfig.publicBaseUrl}/api/v1/campus-images/${filename}`
    });
  } catch (error) {
    next(error);
  }
});

// GET: Retrieve the list of campus images with URLs
router.get('/campus-images', (req, res) => {
  try {
    const files = getCampusImagesList();
    const images = files.map(file => ({
      filename: file,
      url: `${chatConfig.publicBaseUrl}/api/v1/campus-images/${file}`
    }));
    return res.json({ images });
  } catch (error) {
    return res.status(500).json({ detail: error.message });
  }
});

// GET: Serve a specific campus image file
router.get('/campus-images/:filename', (req, res) => {
  const filename = req.params.filename;
  if (filename.includes('..') || filename.includes('/') || filename.includes('\\')) {
    return res.status(400).json({ detail: 'Invalid filename' });
  }
  const imagePath = path.resolve('storage', 'campus', filename);
  if (!fs.existsSync(imagePath)) {
    return res.status(404).json({ detail: 'Image not found' });
  }
  return res.sendFile(imagePath);
});

// DELETE: Delete a specific campus image from the list
router.delete('/campus-images/:filename', (req, res) => {
  const filename = req.params.filename;
  if (filename.includes('..') || filename.includes('/') || filename.includes('\\')) {
    return res.status(400).json({ detail: 'Invalid filename' });
  }
  const imagePath = path.resolve('storage', 'campus', filename);
  if (!fs.existsSync(imagePath)) {
    return res.status(404).json({ detail: 'Image not found' });
  }
  fs.unlinkSync(imagePath);
  return res.json({ detail: 'Image deleted successfully' });
});

// POST: Backward-compatible single image upload
router.post('/campus-image', uploadImage.single('file'), (req, res, next) => {
  try {
    if (!req.file) {
      return res.status(400).json({ detail: 'file is required' });
    }
    if (!req.file.mimetype.startsWith('image/')) {
      return res.status(400).json({ detail: 'file must be an image' });
    }

    const storageDir = path.resolve('storage');
    if (!fs.existsSync(storageDir)) {
      fs.mkdirSync(storageDir, { recursive: true });
    }

    const imagePath = path.join(storageDir, 'campus.jpg');
    fs.writeFileSync(imagePath, req.file.buffer);

    // Also copy to the list so it appears there
    const campusDir = path.resolve('storage', 'campus');
    if (!fs.existsSync(campusDir)) {
      fs.mkdirSync(campusDir, { recursive: true });
    }
    const filename = `image_${Date.now()}_legacy.jpg`;
    fs.writeFileSync(path.join(campusDir, filename), req.file.buffer);

    return res.json({
      detail: 'Image uploaded successfully',
      url: `${chatConfig.publicBaseUrl}/api/v1/campus-image`
    });
  } catch (error) {
    next(error);
  }
});

// GET: Backward-compatible single image retrieval (returns the latest image or fallback)
router.get('/campus-image', (req, res) => {
  const files = getCampusImagesList();
  if (files.length > 0) {
    // Return the latest one (which is the last one in chronological sort)
    const latestFile = files[files.length - 1];
    return res.sendFile(path.resolve('storage', 'campus', latestFile));
  }

  const legacyPath = path.resolve('storage', 'campus.jpg');
  if (!fs.existsSync(legacyPath)) {
    return res.status(404).json({ detail: 'Campus image not found' });
  }
  return res.sendFile(legacyPath);
});

router.get('/chat/sessions', async (req, res, next) => {
  try {
    const limit = clampInt(req.query.limit, 50, 1, 500);
    res.json(await getSessions(limit));
  } catch (error) {
    next(error);
  }
});

router.get('/chat/history/:sessionId', async (req, res, next) => {
  try {
    const limit = clampInt(req.query.limit, 50, 1, 500);
    res.json(await getMessages(req.params.sessionId, limit));
  } catch (error) {
    next(error);
  }
});

router.post('/chat/text', async (req, res, next) => {
  try {
    const message = String(req.body?.message || '').trim();
    if (!message) {
      return res.status(400).json({ detail: 'message is required' });
    }

    const sessionId = String(req.body?.session_id || randomUUID());
    const history = await getMessages(sessionId);
    const replyText = await generateReply(message, sessionId, history);
    await appendMessage(sessionId, 'user', message);
    await appendMessage(sessionId, 'assistant', replyText);
    const audioUrl = await synthesizeReply(replyText);

    return res.json({
      session_id: sessionId,
      input_text: message,
      reply_text: replyText,
      audio_url: audioUrl,
    });
  } catch (error) {
    next(error);
  }
});

router.post('/chat/voice', upload.single('file'), async (req, res, next) => {
  try {
    if (!req.file) {
      return res.status(400).json({ detail: 'file is required' });
    }

    ensureStorageDirs();
    const savedPath = saveVoiceUpload(req.file);
    const sessionId = String(req.body?.session_id || randomUUID());
    let transcript = '';
    try {
      transcript = await transcribe(
        req.file.buffer,
        req.file.originalname || 'recording.wav',
        req.file.mimetype || 'application/octet-stream'
      );
    } catch (error) {
      if (isAsrError(error)) {
        return res.status(422).json({
          detail: {
            message: 'FPT ASR could not transcribe uploaded audio',
            error: error.message,
            saved_voice_file: savedPath,
            filename: req.file.originalname || 'recording.wav',
            content_type: req.file.mimetype || 'application/octet-stream',
            size: req.file.size,
          },
        });
      }
      throw error;
    }
    if (!transcript) {
      return res.status(422).json({
        detail: {
          message: 'FPT ASR response did not include transcript',
          saved_voice_file: savedPath,
          size: req.file.size,
        },
      });
    }

    const history = await getMessages(sessionId);
    const replyText = await generateReply(transcript, sessionId, history);
    await appendMessage(sessionId, 'user', transcript);
    await appendMessage(sessionId, 'assistant', replyText);
    const audioUrl = await synthesizeReply(replyText);

    return res.json({
      session_id: sessionId,
      input_text: transcript,
      transcript,
      reply_text: replyText,
      audio_url: audioUrl,
    });
  } catch (error) {
    next(error);
  }
});

router.get('/audio/:filename', (req, res) => {
  const audioPath = resolveAudioPath(req.params.filename);
  if (!audioPath) {
    return res.status(404).json({ detail: 'Audio file not found' });
  }
  return res.sendFile(audioPath);
});

router.post('/asr/fpt', upload.single('file'), async (req, res, next) => {
  try {
    if (!req.file) {
      return res.status(400).json({ detail: 'file is required' });
    }
    const raw = await transcribeRaw(req.file);
    return res.json({
      filename: req.file.originalname || 'recording.wav',
      content_type: req.file.mimetype || 'application/octet-stream',
      size: req.file.size,
      transcript: extractTranscript(raw),
      raw,
    });
  } catch (error) {
    next(error);
  }
});

router.post('/tts/viettel', handleTtsRequest);
router.post('/tts/fpt', handleTtsRequest);

async function handleTtsRequest(req, res, next) {
  try {
    const text = String(req.body?.text || '').trim();
    if (!text) {
      return res.status(400).json({ detail: 'text is required' });
    }
    const audio = await synthesizeWithViettel(text, {
      voice: req.body?.voice,
      speed: req.body?.speed,
      tts_return_option: req.body?.tts_return_option,
      without_filter: req.body?.without_filter,
    });
    const filename = await saveReplyAudio(audio || Buffer.alloc(0));
    return res.json({
      text,
      voice: req.body?.voice || chatConfig.viettelTtsVoice,
      speed: req.body?.speed ?? chatConfig.viettelTtsSpeed,
      tts_return_option: req.body?.tts_return_option ?? chatConfig.viettelTtsReturnOption,
      audio_url: publicAudioUrl(filename),
      size: audio?.length || 0,
    });
  } catch (error) {
    next(error);
  }
}

router.post('/documents/upload', upload.single('file'), async (req, res, next) => {
  try {
    if (!req.file) {
      return res.status(400).json({ detail: 'file is required' });
    }
    const uploaded = await uploadOpenAiDocument(req.file);
    const result = await query(
      `
        insert into documents (
          filename, content_type, size, file_id, vector_store_id, vector_store_file_id, status
        )
        values ($1, $2, $3, $4, $5, $6, $7)
        returning *
      `,
      [
        uploaded.filename,
        uploaded.content_type,
        uploaded.size,
        uploaded.file_id,
        uploaded.vector_store_id,
        uploaded.vector_store_file_id,
        uploaded.status,
      ]
    );
    return res.json(documentResponse(result.rows[0]));
  } catch (error) {
    next(error);
  }
});

router.get('/documents', async (_req, res, next) => {
  try {
    await syncOpenAiDocumentStatuses();
    const result = await query('select * from documents order by updated_at desc, id desc');
    res.json(result.rows.map(documentResponse));
  } catch (error) {
    next(error);
  }
});

async function syncOpenAiDocumentStatuses() {
  const files = await listOpenAiVectorStoreFiles();
  if (files.length === 0) {
    return;
  }

  for (const file of files) {
    if (!file.id || !file.status) {
      continue;
    }
    await query(
      `
        update documents
        set status = $1,
            updated_at = now()
        where file_id = $2 and status is distinct from $1
      `,
      [file.status, file.id]
    );
  }
}

router.put('/documents/:documentId', upload.single('file'), async (req, res, next) => {
  try {
    if (!req.file) {
      return res.status(400).json({ detail: 'file is required' });
    }
    const existing = await query('select * from documents where id = $1', [req.params.documentId]);
    if (!existing.rows[0]) {
      return res.status(404).json({ detail: 'Document not found' });
    }

    await deleteOpenAiDocument(existing.rows[0].vector_store_id, existing.rows[0].file_id);
    const uploaded = await uploadOpenAiDocument(req.file);
    const result = await query(
      `
        update documents
        set filename = $1,
            content_type = $2,
            size = $3,
            file_id = $4,
            vector_store_id = $5,
            vector_store_file_id = $6,
            status = $7,
            updated_at = now()
        where id = $8
        returning *
      `,
      [
        uploaded.filename,
        uploaded.content_type,
        uploaded.size,
        uploaded.file_id,
        uploaded.vector_store_id,
        uploaded.vector_store_file_id,
        uploaded.status,
        req.params.documentId,
      ]
    );
    return res.json(documentResponse(result.rows[0]));
  } catch (error) {
    next(error);
  }
});

router.delete('/documents/:documentId', async (req, res, next) => {
  try {
    const existing = await query('select * from documents where id = $1', [req.params.documentId]);
    if (!existing.rows[0]) {
      return res.status(404).json({ detail: 'Document not found' });
    }
    await deleteOpenAiDocument(existing.rows[0].vector_store_id, existing.rows[0].file_id);
    const result = await query('delete from documents where id = $1', [req.params.documentId]);
    return res.json({ id: Number(req.params.documentId), deleted: result.rowCount > 0 });
  } catch (error) {
    next(error);
  }
});

router.get('/admin/chat/sessions', async (req, res, next) => {
  try {
    res.json(await getSessions(clampInt(req.query.limit, 100, 1, 500)));
  } catch (error) {
    next(error);
  }
});

router.get('/admin/chat/history/:sessionId', async (req, res, next) => {
  try {
    const messages = await getMessages(req.params.sessionId, clampInt(req.query.limit, 100, 1, 500));
    if (messages.length === 0) {
      return res.status(404).json({ detail: 'Chat session not found' });
    }
    return res.json(messages);
  } catch (error) {
    next(error);
  }
});

router.get('/admin/stats/usage', async (req, res, next) => {
  try {
    const period = ['day', 'week', 'month'].includes(req.query.period) ? req.query.period : 'day';
    res.json(await usageStats(period));
  } catch (error) {
    next(error);
  }
});

router.get('/admin/stats/top-questions', async (req, res, next) => {
  try {
    res.json(await topQuestions(clampInt(req.query.limit, 20, 1, 100)));
  } catch (error) {
    next(error);
  }
});

router.get('/admin/stats/unanswered', async (req, res, next) => {
  try {
    res.json(await unansweredQuestions(clampInt(req.query.limit, 50, 1, 200)));
  } catch (error) {
    next(error);
  }
});

async function synthesizeReply(replyText) {
  try {
    const audio = await synthesize(replyText);
    if (!audio) {
      return null;
    }
    return publicAudioUrl(await saveReplyAudio(audio));
  } catch (error) {
    console.warn('TTS synthesize failed; returning text only:', error.message);
    return null;
  }
}

async function transcribeRaw(file) {
  return transcribeRawWithFpt(
    file.buffer,
    file.originalname || 'recording.wav',
    file.mimetype || 'application/octet-stream'
  );
}

function saveVoiceUpload(file) {
  const suffix = path.extname(file.originalname || '').toLowerCase() || '.wav';
  const allowedSuffix = ['.wav', '.mp3', '.m4a', '.aac', '.raw', '.pcm'].includes(suffix) ? suffix : '.wav';
  const timestamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\.\d{3}Z$/, 'Z');
  const filename = `voice_${timestamp}_${randomUUID().replaceAll('-', '')}${allowedSuffix}`;
  const savedPath = path.join(chatConfig.voiceUploadDir, filename);
  fs.writeFileSync(savedPath, file.buffer);
  return savedPath;
}

function isAsrError(error) {
  return (
    error?.statusCode ||
    error?.message?.includes('FPT ASR') ||
    error?.message?.includes('transcript')
  );
}

function documentResponse(row) {
  return {
    id: Number(row.id),
    filename: row.filename,
    content_type: row.content_type,
    size: Number(row.size),
    file_id: row.file_id,
    vector_store_id: row.vector_store_id,
    vector_store_file_id: row.vector_store_file_id,
    status: row.status,
    created_at: row.created_at,
    updated_at: row.updated_at,
  };
}

function clampInt(value, defaultValue, min, max) {
  const parsed = Number.parseInt(value, 10);
  if (!Number.isInteger(parsed)) {
    return defaultValue;
  }
  return Math.min(max, Math.max(min, parsed));
}

export default router;
