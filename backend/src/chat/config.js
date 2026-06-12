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
  openaiRecentHistoryLimit: Number.parseInt(process.env.OPENAI_RECENT_HISTORY_LIMIT || '6', 10),
  openaiMaxOutputTokens: Number.parseInt(process.env.OPENAI_MAX_OUTPUT_TOKENS || '800', 10),
  openaiRetryMaxOutputTokens: Number.parseInt(process.env.OPENAI_RETRY_MAX_OUTPUT_TOKENS || '1400', 10),
  openaiReasoningEffort: process.env.OPENAI_REASONING_EFFORT || 'low',
  openaiTextVerbosity: process.env.OPENAI_TEXT_VERBOSITY || 'low',
  openaiFileSearchMaxResults: Number.parseInt(process.env.OPENAI_FILE_SEARCH_MAX_RESULTS || '8', 10),
  openaiVectorFilePollAttempts: Number.parseInt(process.env.OPENAI_VECTOR_FILE_POLL_ATTEMPTS || '45', 10),
  openaiVectorFilePollIntervalMs: Math.round(Number.parseFloat(process.env.OPENAI_VECTOR_FILE_POLL_INTERVAL_SECONDS || '2') * 1000),
  openaiVectorStoreId: process.env.OPENAI_VECTOR_STORE_ID || '',
  openaiVectorStoreIdFile: process.env.OPENAI_VECTOR_STORE_ID_FILE || 'storage/openai_vector_store_id.txt',
  openaiVectorStoreName: process.env.OPENAI_VECTOR_STORE_NAME || 'fpt-university-regulations',
  fptAsrUrl: process.env.FPT_ASR_URL || 'https://api.fpt.ai/hmi/asr/general',
  fptAsrApiKey: process.env.FPT_ASR_API_KEY || '',
  viettelTtsUrl: process.env.VIETTEL_TTS_URL || 'https://viettelai.vn/tts/speech_synthesis',
  viettelTtsToken: process.env.VIETTEL_TTS_TOKEN || '',
  viettelTtsVoice: process.env.VIETTEL_TTS_VOICE || 'hcm-diemmy',
  viettelTtsSpeed: Number.parseFloat(process.env.VIETTEL_TTS_SPEED || '1.0'),
  viettelTtsReturnOption: Number.parseInt(process.env.VIETTEL_TTS_RETURN_OPTION || '2', 10),
  viettelTtsWithoutFilter: parseBoolean(process.env.VIETTEL_TTS_WITHOUT_FILTER, false),
};

function parseBoolean(value, defaultValue) {
  if (value === undefined || value === '') {
    return defaultValue;
  }
  return ['1', 'true', 'yes', 'on'].includes(String(value).toLowerCase());
}

export const SYSTEM_INSTRUCTIONS = `
Bạn là UniMate, trợ lý sinh viên của FPT University.
Trả lời bằng tiếng Việt có dấu, ngắn gọn, rõ ràng.
Nếu câu hỏi liên quan đến quy chế, quy định, học vụ, học phí, lịch thi, thực tập,
chương trình học, môn học, mã môn, học phần, tín chỉ, course, syllabus,
hoặc chính sách nội bộ, chỉ được trả lời dựa trên tài liệu được tìm thấy bằng file_search.
Khi có file_search, hãy tự tạo truy vấn tìm kiếm phù hợp từ câu hỏi của người dùng,
bao gồm cách viết có dấu/không dấu, tiếng Việt/tiếng Anh, viết tắt và cách gọi tương đương.
Nếu không tìm thấy căn cứ trong tài liệu, hãy nói: "Mình chưa có thông tin này trong tài liệu quy chế được cung cấp."
Không bịa số liệu, mốc thời gian, điều khoản, mức phí, điều kiện, hay quy trình.
`.trim();
