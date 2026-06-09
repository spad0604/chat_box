import pg from 'pg';

const { Pool } = pg;

let pool;

export function getPool() {
  if (!pool) {
    throw new Error('Database has not been initialized');
  }
  return pool;
}

export async function query(text, params = []) {
  return getPool().query(text, params);
}

export async function connectDB(databaseUrl) {
  if (!databaseUrl) {
    throw new Error('DATABASE_URL is required');
  }

  pool = new Pool({
    connectionString: databaseUrl,
    ssl: { rejectUnauthorized: false },
  });

  await pool.query('select 1');
  await ensureSchema();
  console.log('Supabase/Postgres connected');
}

async function ensureSchema() {
  await query('create extension if not exists pgcrypto');

  await query(`
    create table if not exists users (
      id uuid primary key default gen_random_uuid(),
      name text not null,
      email text not null unique,
      password_hash text not null,
      role text not null default 'user' check (role in ('user', 'admin')),
      created_at timestamptz not null default now(),
      updated_at timestamptz not null default now()
    )
  `);

  await query(`
    create table if not exists feedback (
      id uuid primary key default gen_random_uuid(),
      user_id uuid references users(id) on delete set null,
      name text not null,
      email text not null,
      phone text not null,
      subject text not null,
      message text not null,
      rating integer not null check (rating between 1 and 5),
      created_at timestamptz not null default now(),
      updated_at timestamptz not null default now()
    )
  `);

  await query(`
    create table if not exists chat_messages (
      id bigserial primary key,
      session_id text not null,
      role text not null check (role in ('user', 'assistant', 'system')),
      content text not null,
      created_at timestamptz not null default now()
    )
  `);

  await query(`
    create index if not exists idx_chat_messages_session_created
    on chat_messages (session_id, created_at, id)
  `);

  await query(`
    create table if not exists documents (
      id bigserial primary key,
      filename text not null,
      content_type text not null,
      size bigint not null,
      file_id text not null,
      vector_store_id text not null,
      vector_store_file_id text,
      status text not null,
      created_at timestamptz not null default now(),
      updated_at timestamptz not null default now()
    )
  `);

  await query(`
    create index if not exists idx_documents_updated
    on documents (updated_at desc, id desc)
  `);
}
