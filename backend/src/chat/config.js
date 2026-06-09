export const chatConfig = {
  appName: process.env.APP_NAME || 'ChatBox Server',
  publicBaseUrl: (process.env.PUBLIC_BASE_URL || `http://localhost:${process.env.PORT || 8000}`).replace(/\/$/, ''),
  llmProvider: process.env.LLM_PROVIDER || 'mock',
  sttProvider: process.env.STT_PROVIDER || 'mock',
  ttsProvider: process.env.TTS_PROVIDER || 'mock',
  audioStorageDir: process.env.AUDIO_STORAGE_DIR || 'storage/audio',
  voiceUploadDir: process.env.VOICE_UPLOAD_DIR || 'storage/uploads',
  openaiApiKey: process.env.OPENAI_API_KEY || '',
  openaiModel: process.env.OPENAI_MODEL || 'gpt-4.1',
  openaiVectorStoreId: process.env.OPENAI_VECTOR_STORE_ID || '',
  openaiVectorStoreIdFile: process.env.OPENAI_VECTOR_STORE_ID_FILE || 'storage/openai_vector_store_id.txt',
  openaiVectorStoreName: process.env.OPENAI_VECTOR_STORE_NAME || 'fpt-university-regulations',
  fptAsrUrl: process.env.FPT_ASR_URL || 'https://api.fpt.ai/hmi/asr/general',
  fptAsrApiKey: process.env.FPT_ASR_API_KEY || '',
  fptTtsUrl: process.env.FPT_TTS_URL || 'https://api.fpt.ai/hmi/tts/v5',
  fptTtsApiKey: process.env.FPT_TTS_API_KEY || process.env.FPT_ASR_API_KEY || '',
  fptTtsVoice: process.env.FPT_TTS_VOICE || 'banmai',
  fptTtsSpeed: process.env.FPT_TTS_SPEED || '',
  fptTtsPollAttempts: Number.parseInt(process.env.FPT_TTS_POLL_ATTEMPTS || '90', 10),
  fptTtsPollIntervalMs: Math.round(Number.parseFloat(process.env.FPT_TTS_POLL_INTERVAL_SECONDS || '1') * 1000),
};

export const SYSTEM_INSTRUCTIONS = `
Ban la UniMate, tro ly sinh vien cua FPT University.
Tra loi bang tieng Viet, ngan gon, ro rang.
Neu cau hoi lien quan den quy che, quy dinh, hoc vu, hoc phi, lich thi, thuc tap,
hoac chinh sach noi bo, chi duoc tra loi dua tren tai lieu duoc tim thay bang file_search.
Neu khong tim thay can cu trong tai lieu, hay noi: "Minh chua co thong tin nay trong tai lieu quy che duoc cung cap."
Khong bia so lieu, moc thoi gian, dieu khoan, muc phi, dieu kien, hay quy trinh.
`.trim();
