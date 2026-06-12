import 'dotenv/config';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { Client } from 'pg';
import { chatConfig } from '../src/chat/config.js';
import { deleteOpenAiDocument } from '../src/chat/providers.js';

const sourceDir = process.argv[2];

if (!sourceDir) {
  console.error('Usage: node scripts/reindexLocalDocuments.js <documents-dir>');
  process.exit(1);
}

if (!fs.existsSync(sourceDir) || !fs.statSync(sourceDir).isDirectory()) {
  console.error(`Document directory not found: ${sourceDir}`);
  process.exit(1);
}

if (!chatConfig.openaiApiKey) {
  console.error('OPENAI_API_KEY is required');
  process.exit(1);
}

const db = new Client({ connectionString: process.env.DATABASE_URL });
await db.connect();

const vectorStoreId = await recreateVectorStore();
const files = fs.readdirSync(sourceDir)
  .filter((name) => !name.startsWith('~$'))
  .filter((name) => /\.(pdf|pptx|docx|txt|md)$/i.test(name))
  .sort((a, b) => a.localeCompare(b, 'vi'));

console.log(`Reindexing ${files.length} documents into ${vectorStoreId}`);

await clearDocumentsTable();

for (const filename of files) {
  const filePath = path.join(sourceDir, filename);
  const originalBuffer = fs.readFileSync(filePath);
  const searchableText = await createSearchableText(filePath, originalBuffer);
  const uploadName = `${path.parse(filename).name}.txt`;
  const uploadBuffer = Buffer.from(searchableText, 'utf8');
  const uploaded = await uploadSearchableText(uploadBuffer, uploadName, vectorStoreId);
  await insertDocument({
    filename,
    contentType: guessContentType(filename),
    size: originalBuffer.length,
    fileId: uploaded.fileId,
    vectorStoreId,
    vectorStoreFileId: uploaded.vectorStoreFileId,
    status: uploaded.status,
  });
  console.log(`  ${filename} -> ${uploaded.status}`);
}

await db.end();
console.log('Reindex complete');

async function recreateVectorStore() {
  const oldVectorStoreId = getCurrentVectorStoreId();
  if (oldVectorStoreId) {
    await deleteVectorStore(oldVectorStoreId);
  }

  const response = await fetch('https://api.openai.com/v1/vector_stores', {
    method: 'POST',
    headers: openAiJsonHeaders(),
    body: JSON.stringify({ name: chatConfig.openaiVectorStoreName }),
  });
  if (!response.ok) {
    throw new Error(`OpenAI vector store create failed: ${await response.text()}`);
  }
  const payload = await response.json();
  fs.mkdirSync(path.dirname(chatConfig.openaiVectorStoreIdFile), { recursive: true });
  fs.writeFileSync(chatConfig.openaiVectorStoreIdFile, payload.id, 'utf8');
  return payload.id;
}

async function deleteVectorStore(vectorStoreId) {
  const response = await fetch(`https://api.openai.com/v1/vector_stores/${vectorStoreId}`, {
    method: 'DELETE',
    headers: { Authorization: `Bearer ${chatConfig.openaiApiKey}` },
  });
  if (!response.ok && response.status !== 404) {
    console.warn(`OpenAI vector store delete skipped: ${await response.text()}`);
  }
}

function getCurrentVectorStoreId() {
  if (chatConfig.openaiVectorStoreId) {
    return chatConfig.openaiVectorStoreId;
  }
  if (fs.existsSync(chatConfig.openaiVectorStoreIdFile)) {
    return fs.readFileSync(chatConfig.openaiVectorStoreIdFile, 'utf8').trim();
  }
  return '';
}

async function clearDocumentsTable() {
  const existing = await db.query('select vector_store_id, file_id from documents');
  for (const row of existing.rows) {
    await deleteOpenAiDocument(row.vector_store_id, row.file_id);
  }
  await db.query('delete from documents');
}

async function createSearchableText(filePath, originalBuffer) {
  const filename = path.basename(filePath);
  const ext = path.extname(filename).toLowerCase();
  let body = '';
  if (ext === '.pptx') {
    body = await extractOfficeOpenXmlText(filePath, 'ppt/slides/slide');
  } else if (ext === '.docx') {
    body = await extractOfficeOpenXmlText(filePath, 'word/document.xml');
  } else if (ext === '.pdf') {
    body = await extractPdfText(filePath);
  } else if (ext === '.txt' || ext === '.md') {
    body = originalBuffer.toString('utf8');
  }

  if (!body.trim()) {
    body = `Tai lieu goc: ${filename}`;
  }

  return [
    `Ten tai lieu: ${filename}`,
    `Tu khoa ten file: ${filenameToKeywords(filename)}`,
    `Tu khoa chu de: ${documentTopicKeywords(filename)}`,
    `Cau hoi co the tra loi tu tai lieu: ${buildDocumentQuestionHints(filename, body)}`,
    '',
    body,
  ].join('\n');
}

async function extractOfficeOpenXmlText(filePath, prefix) {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'unimate_ooxml_'));
  const zipPath = path.join(tempDir, 'source.zip');
  try {
    fs.copyFileSync(filePath, zipPath);
    await runCommand('powershell', [
      '-NoProfile',
      '-Command',
      `Expand-Archive -LiteralPath ${quotePowerShell(zipPath)} -DestinationPath ${quotePowerShell(tempDir)} -Force`,
    ]);
    const files = listFiles(tempDir)
      .filter((item) => item.replaceAll('\\', '/').includes(prefix))
      .filter((item) => item.toLowerCase().endsWith('.xml'))
      .sort(naturalSort);
    return files.map((xmlPath, index) => {
      const text = extractTextFromXml(fs.readFileSync(xmlPath, 'utf8'));
      return text ? `--- Phan ${index + 1} ---\n${text}` : '';
    }).filter(Boolean).join('\n\n');
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
}

async function extractPdfText(filePath) {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'unimate_pdf_'));
  const outputPath = path.join(tempDir, 'document.txt');
  try {
    await runCommand('pdftotext', ['-layout', '-enc', 'UTF-8', filePath, outputPath]);
    return fs.readFileSync(outputPath, 'utf8')
      .replace(/\f/g, '\n\n')
      .replace(/\n{4,}/g, '\n\n\n')
      .trim();
  } catch (error) {
    console.warn(`PDF text extraction skipped for ${path.basename(filePath)}: ${error.message}`);
    return '';
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
}

function extractTextFromXml(xml) {
  return [...xml.matchAll(/<a:t[^>]*>([\s\S]*?)<\/a:t>|<w:t[^>]*>([\s\S]*?)<\/w:t>/g)]
    .map((match) => decodeXml(match[1] || match[2] || ''))
    .join('\n')
    .replace(/\n[ \t]+/g, '\n')
    .replace(/(?<![.!?:;…)\]”"]) *\n(?!\s*(---|\d+\.|[A-ZÀ-Ỵ][^a-zà-ỹ]{8,}$))/gu, ' ')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function buildDocumentQuestionHints(filename, body) {
  const hints = [
    filenameToKeywords(filename),
    documentTopicKeywords(filename),
  ];
  if (/ngôn ngữ nhật|ngon ngu nhat|JPD116|JPD126|OJP202/i.test(`${filename}\n${body}`)) {
    hints.push('ngành ngôn ngữ Nhật kỳ 1 học những gì');
    hints.push('mã học phần ngành ngôn ngữ Nhật JPD116 JPD126 OJP202');
    hints.push('môn học có chữ c sau cùng triển khai trên Coursera');
    hints.push('môn Coursera ký hiệu chữ c cuối mã học phần');
  }
  if (/coursera/i.test(body)) {
    hints.push('Coursera học online môn học course chữ c mã học phần');
  }
  return [...new Set(hints)].join(' | ');
}

function documentTopicKeywords(filename) {
  const normalized = filenameToKeywords(filename).toLowerCase();
  if (normalized.includes('noi quy sinh vien')) {
    return 'nội quy, kỷ luật, phòng thi, giám thị, phúc khảo, vi phạm, cảnh cáo, đình chỉ, thôi học';
  }
  if (normalized.includes('qd 1234') || normalized.includes('noi quy ky thi')) {
    return 'kỳ thi, phòng thi, giám thị, phúc khảo, sao chép, gian lận, điểm thi, điểm danh phòng thi, thời gian phúc khảo, hình thức xử lý gian lận';
  }
  if (normalized.includes('fqa') || normalized.includes('thu tuc don tu')) {
    return 'thủ tục, đơn từ, hành chính, nộp đơn, tiền học, bảo lưu, chuyển ngành, nghỉ học tạm thời, cấp lại giấy tờ, học phí, Phòng Tài vụ, Phòng Tuyển sinh, FAQ';
  }
  if (normalized.includes('dinh huong')) {
    return 'định hướng, sinh viên mới, kỳ 1, ngành, khoa, chương trình đào tạo, thư viện, LMS, email sinh viên, hoạt động ngoại khóa, năm nhất, giới thiệu trường';
  }
  if (normalized.includes('qd 303') || normalized.includes('quy che dao tao')) {
    return 'quy chế đào tạo, khóa luận tốt nghiệp, đăng ký đề tài, giảng viên hướng dẫn, hội đồng chấm, bảo vệ, chấm điểm, học lại, tích lũy, tốt nghiệp, hạng tốt nghiệp, bằng tốt nghiệp, tín chỉ, OJT, nghĩa vụ tài chính, học phí, Giáo dục quốc phòng, Giáo dục thể chất, điểm trung bình, phúc khảo, đại học chính quy';
  }
  return '';
}

function decodeXml(value) {
  return value
    .replaceAll('&lt;', '<')
    .replaceAll('&gt;', '>')
    .replaceAll('&amp;', '&')
    .replaceAll('&quot;', '"')
    .replaceAll('&apos;', "'");
}

function listFiles(dir) {
  const results = [];
  for (const item of fs.readdirSync(dir, { withFileTypes: true })) {
    const fullPath = path.join(dir, item.name);
    if (item.isDirectory()) {
      results.push(...listFiles(fullPath));
    } else {
      results.push(fullPath);
    }
  }
  return results;
}

function naturalSort(a, b) {
  return a.localeCompare(b, undefined, { numeric: true });
}

function quotePowerShell(value) {
  return `'${value.replaceAll("'", "''")}'`;
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

function filenameToKeywords(filename) {
  return filename
    .replace(/\.[^.]+$/, '')
    .replace(/[()_.-]+/g, ' ')
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .trim();
}

async function uploadSearchableText(buffer, filename, vectorStoreId) {
  const form = new FormData();
  form.set('purpose', 'assistants');
  form.set('file', new Blob([buffer], { type: 'text/plain; charset=utf-8' }), filename);

  const fileResponse = await fetchWithRetry('https://api.openai.com/v1/files', {
    method: 'POST',
    headers: { Authorization: `Bearer ${chatConfig.openaiApiKey}` },
    body: form,
  });
  if (!fileResponse.ok) {
    throw new Error(`OpenAI file upload failed: ${await fileResponse.text()}`);
  }
  const filePayload = await fileResponse.json();

  const attachResponse = await fetchWithRetry(`https://api.openai.com/v1/vector_stores/${vectorStoreId}/files`, {
    method: 'POST',
    headers: openAiJsonHeaders(),
    body: JSON.stringify({ file_id: filePayload.id }),
  });
  if (!attachResponse.ok) {
    throw new Error(`OpenAI vector attach failed: ${await attachResponse.text()}`);
  }
  const vectorFile = await waitForVectorStoreFile(vectorStoreId, (await attachResponse.json()).id);
  return {
    fileId: filePayload.id,
    vectorStoreFileId: vectorFile.id,
    status: vectorFile.status || 'completed',
  };
}

async function waitForVectorStoreFile(vectorStoreId, vectorStoreFileId) {
  for (let attempt = 0; attempt < 60; attempt += 1) {
    const response = await fetch(`https://api.openai.com/v1/vector_stores/${vectorStoreId}/files/${vectorStoreFileId}`, {
      headers: { Authorization: `Bearer ${chatConfig.openaiApiKey}` },
    });
    if (!response.ok) {
      throw new Error(`OpenAI vector file status failed: ${await response.text()}`);
    }
    const payload = await response.json();
    if (payload.status === 'completed') {
      return payload;
    }
    if (['failed', 'cancelled'].includes(payload.status)) {
      throw new Error(`OpenAI vector file indexing ${payload.status}: ${JSON.stringify(payload.last_error || payload)}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 2000));
  }
  throw new Error(`OpenAI vector file indexing timed out: ${vectorStoreFileId}`);
}

async function insertDocument(document) {
  await db.query(
    `
      insert into documents (
        filename, content_type, size, file_id, vector_store_id, vector_store_file_id, status
      )
      values ($1, $2, $3, $4, $5, $6, $7)
    `,
    [
      document.filename,
      document.contentType,
      document.size,
      document.fileId,
      document.vectorStoreId,
      document.vectorStoreFileId,
      document.status,
    ]
  );
}

function openAiJsonHeaders() {
  return {
    Authorization: `Bearer ${chatConfig.openaiApiKey}`,
    'Content-Type': 'application/json',
  };
}

async function fetchWithRetry(url, options) {
  let response;
  for (let attempt = 0; attempt < 5; attempt += 1) {
    response = await fetch(url, options);
    if (![429, 500, 502, 503, 504].includes(response.status)) {
      return response;
    }
    await new Promise((resolve) => setTimeout(resolve, 1000 * (attempt + 1)));
  }
  return response;
}

function guessContentType(filename) {
  const ext = path.extname(filename).toLowerCase();
  if (ext === '.pdf') return 'application/pdf';
  if (ext === '.pptx') return 'application/vnd.openxmlformats-officedocument.presentationml.presentation';
  if (ext === '.docx') return 'application/vnd.openxmlformats-officedocument.wordprocessingml.document';
  if (ext === '.txt') return 'text/plain';
  if (ext === '.md') return 'text/markdown';
  return 'application/octet-stream';
}
