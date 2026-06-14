import 'dotenv/config';
import fs from 'node:fs';
import path from 'node:path';
import { Client } from 'pg';
import { chatConfig } from '../src/chat/config.js';
import { createSearchableDocument } from '../src/chat/documentSearch.js';
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
  const searchableDocument = await createSearchableDocument({
    buffer: originalBuffer,
    filename,
    displayName: filename,
    mimetype: guessContentType(filename),
  });
  const uploaded = await uploadSearchableText(searchableDocument.buffer, searchableDocument.uploadName, vectorStoreId);
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
    if (![429, 500, 502, 503, 504, 520].includes(response.status)) {
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
