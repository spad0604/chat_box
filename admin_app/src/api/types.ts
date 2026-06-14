export type Period = "day" | "week" | "month";

export type UsageBucket = {
  bucket: string;
  total_messages: number;
  user_messages: number;
  assistant_messages: number;
  sessions: number;
};

export type UsageStats = {
  period: Period;
  total_messages: number;
  user_messages: number;
  assistant_messages: number;
  sessions: number;
  buckets: UsageBucket[];
};

export type ChatSession = {
  session_id: string;
  updated_at: string;
  last_message: string;
};

export type ChatMessage = {
  role: "user" | "assistant" | "system" | string;
  content: string;
  created_at: string;
};

export type TopQuestion = {
  question: string;
  count: number;
  last_asked_at: string;
};

export type UnansweredQuestion = {
  session_id: string;
  question: string;
  answer: string;
  asked_at: string;
};

export type AdminDocument = {
  id: number;
  filename: string;
  content_type: string;
  size: number;
  file_id: string;
  vector_store_id: string;
  vector_store_file_id?: string | null;
  status: string;
  created_at: string;
  updated_at: string;
};

export type CampusImage = {
  filename: string;
  url: string;
};

export type CampusImagesResponse = {
  images: CampusImage[];
};
