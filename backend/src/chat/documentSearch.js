import AdmZip from 'adm-zip';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

export async function createSearchableDocument({ buffer, filename, displayName = filename, mimetype = '' }) {
  const sourceName = filename || displayName || 'document';
  const publicName = displayName || sourceName;
  const documentBuffer = toBuffer(buffer);
  const body = await extractDocumentText(documentBuffer, sourceName, mimetype);
  const searchableText = buildSearchableText(publicName, body);
  return {
    buffer: Buffer.from(searchableText, 'utf8'),
    uploadName: `${path.parse(publicName).name || 'document'}.txt`,
    mimetype: 'text/plain; charset=utf-8',
    text: searchableText,
  };
}

export async function extractDocumentText(buffer, filename, mimetype = '') {
  const documentBuffer = toBuffer(buffer);
  const ext = path.extname(filename || '').toLowerCase();
  if (ext === '.pptx') {
    return extractOfficeOpenXmlText(documentBuffer, 'ppt/slides/slide');
  }
  if (ext === '.docx') {
    return extractOfficeOpenXmlText(documentBuffer, 'word/document.xml');
  }
  if (ext === '.pdf' || mimetype.toLowerCase().includes('pdf')) {
    return extractPdfText(documentBuffer, filename);
  }
  if (isTextLikeDocument(filename, mimetype)) {
    return decodeTextBuffer(documentBuffer);
  }
  return '';
}

export function buildSearchableText(filename, body) {
  const safeBody = body.trim() || `Tai lieu goc: ${filename}`;
  return [
    `Ten tai lieu: ${filename}`,
    `Tu khoa ten file: ${filenameToKeywords(filename)}`,
    `Tu khoa chu de: ${documentTopicKeywords(filename)}`,
    `Cau hoi co the tra loi tu tai lieu: ${buildDocumentQuestionHints(filename, safeBody)}`,
    '',
    safeBody,
  ].join('\n');
}

export function buildDocumentQuestionHints(filename, body = '') {
  const hints = [
    filenameToKeywords(filename),
    documentTopicKeywords(filename),
  ].filter(Boolean);
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

export function documentTopicKeywords(filename) {
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

export function filenameToKeywords(filename) {
  return String(filename || '')
    .replace(/\.[^.]+$/, '')
    .replace(/[()_.-]+/g, ' ')
    .normalize('NFD')
    .replace(/[\u0300-\u036f]/g, '')
    .replace(/[đĐ]/g, 'd')
    .trim();
}

function extractOfficeOpenXmlText(buffer, prefix) {
  const zip = new AdmZip(buffer);
  const entries = zip.getEntries()
    .filter((entry) => !entry.isDirectory)
    .filter((entry) => entry.entryName.replaceAll('\\', '/').includes(prefix))
    .filter((entry) => entry.entryName.toLowerCase().endsWith('.xml'))
    .sort((a, b) => naturalSort(a.entryName, b.entryName));

  return entries.map((entry, index) => {
    const text = extractTextFromXml(entry.getData().toString('utf8'));
    return text ? `--- Phan ${index + 1} ---\n${text}` : '';
  }).filter(Boolean).join('\n\n');
}

function toBuffer(value) {
  if (Buffer.isBuffer(value)) {
    return value;
  }
  if (value instanceof ArrayBuffer) {
    return Buffer.from(value);
  }
  if (ArrayBuffer.isView(value)) {
    return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  }
  return Buffer.from(value || []);
}

async function extractPdfText(buffer, filename) {
  const tempDir = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'unimate_pdf_'));
  const inputPath = path.join(tempDir, path.basename(filename || 'document.pdf'));
  const outputPath = path.join(tempDir, 'document.txt');
  try {
    await fs.promises.writeFile(inputPath, buffer);
    await runCommand(process.env.PDFTOTEXT_PATH || 'pdftotext', ['-layout', '-enc', 'UTF-8', inputPath, outputPath]);
    return (await fs.promises.readFile(outputPath, 'utf8'))
      .replace(/\f/g, '\n\n')
      .replace(/\n{4,}/g, '\n\n\n')
      .trim();
  } catch (error) {
    const wrapped = new Error(`Không thể tách nội dung PDF "${filename}". Vui lòng cài pdftotext/poppler-utils trên server hoặc cấu hình PDFTOTEXT_PATH. Chi tiết: ${error.message}`);
    wrapped.statusCode = 400;
    throw wrapped;
  } finally {
    await fs.promises.rm(tempDir, { recursive: true, force: true });
  }
}

function extractTextFromXml(xml) {
  return [...xml.matchAll(/<a:t(?:\s[^>]*)?>([\s\S]*?)<\/a:t>|<w:t(?:\s[^>]*)?>([\s\S]*?)<\/w:t>/g)]
    .map((match) => decodeXml(match[1] || match[2] || ''))
    .join('\n')
    .replace(/\n[ \t]+/g, '\n')
    .replace(/(?<![.!?:;…)\]”"]) *\n(?!\s*(---|\d+\.|[A-ZÀ-Ỵ][^a-zà-ỹ]{8,}$))/gu, ' ')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function decodeXml(value) {
  return value
    .replaceAll('&lt;', '<')
    .replaceAll('&gt;', '>')
    .replaceAll('&amp;', '&')
    .replaceAll('&quot;', '"')
    .replaceAll('&apos;', "'");
}

function naturalSort(a, b) {
  return a.localeCompare(b, undefined, { numeric: true });
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

function isTextLikeDocument(filename, mimetype) {
  const lowerName = String(filename || '').toLowerCase();
  const lowerType = String(mimetype || '').toLowerCase();
  if (lowerType.includes('openxmlformats-officedocument')) {
    return false;
  }
  return (
    lowerType.startsWith('text/') ||
    lowerType.includes('json') ||
    lowerType === 'application/xml' ||
    lowerType.endsWith('+xml') ||
    ['.txt', '.md', '.csv', '.json', '.xml', '.html', '.htm'].includes(path.extname(lowerName))
  );
}

function decodeTextBuffer(buffer) {
  const encoding = detectTextEncoding(buffer);
  try {
    return new TextDecoder(encoding, { fatal: false }).decode(buffer).replace(/^\uFEFF/, '');
  } catch {
    return buffer.toString('utf8').replace(/^\uFEFF/, '');
  }
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
