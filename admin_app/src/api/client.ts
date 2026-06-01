import type {
  AdminDocument,
  ChatMessage,
  ChatSession,
  Period,
  TopQuestion,
  UnansweredQuestion,
  UsageStats
} from "./types";

export const API_BASE_URL = "https://etching-underhand-placidly.ngrok-free.dev/api/v1";

async function requestJson<T>(path: string, init?: RequestInit): Promise<T> {
  const headers = new Headers(init?.headers);
  headers.set("ngrok-skip-browser-warning", "true");
  headers.set("Accept", "application/json");

  const url = `${API_BASE_URL}${path}`;
  let response: Response;
  try {
    response = await fetch(url, {
      ...init,
      mode: "cors",
      credentials: "omit",
      headers
    });
  } catch (error) {
    console.error("API network error", { url, error });
    throw error;
  }
  const text = await response.text();
  const payload = text ? JSON.parse(text) : null;

  if (!response.ok) {
    const detail = payload?.detail;
    const message = typeof detail === "string" ? detail : JSON.stringify(detail ?? payload);
    throw new Error(message || `Request failed: ${response.status}`);
  }

  return payload as T;
}

export function fetchUsage(period: Period): Promise<UsageStats> {
  return requestJson<UsageStats>(`/admin/stats/usage?period=${period}`);
}

export function fetchSessions(): Promise<ChatSession[]> {
  return requestJson<ChatSession[]>("/admin/chat/sessions?limit=100");
}

export function fetchHistory(sessionId: string): Promise<ChatMessage[]> {
  return requestJson<ChatMessage[]>(`/admin/chat/history/${encodeURIComponent(sessionId)}?limit=100`);
}

export function fetchTopQuestions(): Promise<TopQuestion[]> {
  return requestJson<TopQuestion[]>("/admin/stats/top-questions?limit=30");
}

export function fetchUnansweredQuestions(): Promise<UnansweredQuestion[]> {
  return requestJson<UnansweredQuestion[]>("/admin/stats/unanswered?limit=100");
}

export function fetchDocuments(): Promise<AdminDocument[]> {
  return requestJson<AdminDocument[]>("/documents");
}

export async function uploadDocument(file: File): Promise<void> {
  const formData = new FormData();
  formData.append("file", file);
  await requestJson("/documents/upload", {
    method: "POST",
    body: formData
  });
}

export async function replaceDocument(documentId: number, file: File): Promise<void> {
  const formData = new FormData();
  formData.append("file", file);
  await requestJson(`/documents/${documentId}`, {
    method: "PUT",
    body: formData
  });
}

export async function deleteDocument(documentId: number): Promise<void> {
  await requestJson(`/documents/${documentId}`, {
    method: "DELETE"
  });
}
