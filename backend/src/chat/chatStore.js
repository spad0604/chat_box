import { query } from '../config/db.js';

export async function appendMessage(sessionId, role, content) {
  await query('insert into chat_messages (session_id, role, content) values ($1, $2, $3)', [
    sessionId,
    role,
    content,
  ]);
}

export async function getMessages(sessionId, limit = 20) {
  const result = await query(
    `
      select role, content, created_at
      from chat_messages
      where session_id = $1
      order by created_at desc, id desc
      limit $2
    `,
    [sessionId, limit]
  );

  return result.rows.reverse().map((row) => ({
    role: row.role,
    content: row.content,
    created_at: row.created_at,
  }));
}

export async function getSessions(limit = 50) {
  const result = await query(
    `
      select distinct on (session_id) session_id, created_at, content
      from chat_messages
      order by session_id, created_at desc, id desc
    `
  );

  return result.rows
    .map((row) => ({
      session_id: row.session_id,
      updated_at: row.created_at,
      last_message: row.content,
    }))
    .sort((left, right) => new Date(right.updated_at) - new Date(left.updated_at))
    .slice(0, limit);
}

export async function usageStats(period) {
  const unit = ['day', 'week', 'month'].includes(period) ? period : 'day';
  const totals = await query(`
    select
      count(*)::int as total_messages,
      count(*) filter (where role = 'user')::int as user_messages,
      count(*) filter (where role = 'assistant')::int as assistant_messages,
      count(distinct session_id)::int as sessions
    from chat_messages
  `);
  const buckets = await query(`
    select
      date_trunc('${unit}', created_at) as bucket,
      count(*)::int as total_messages,
      count(*) filter (where role = 'user')::int as user_messages,
      count(*) filter (where role = 'assistant')::int as assistant_messages,
      count(distinct session_id)::int as sessions
    from chat_messages
    group by bucket
    order by bucket desc
    limit 30
  `);

  return {
    period,
    ...totals.rows[0],
    buckets: buckets.rows,
  };
}

export async function topQuestions(limit = 20) {
  const result = await query(
    `
      select trim(content) as question, count(*)::int as count, max(created_at) as last_asked_at
      from chat_messages
      where role = 'user' and length(trim(content)) > 0
      group by lower(trim(content)), trim(content)
      order by count desc, last_asked_at desc
      limit $1
    `,
    [limit]
  );
  return result.rows;
}

export async function unansweredQuestions(limit = 50) {
  const markers = [
    'minh chua co thong tin',
    'mình chưa có thông tin',
    'chua co thong tin',
    'chưa có thông tin',
    'khong tim thay',
    'không tìm thấy',
  ];
  const conditions = markers.map((_, index) => `lower(answer) like $${index + 1}`).join(' or ');
  const result = await query(
    `
      with ordered as (
        select
          session_id,
          role,
          content,
          created_at,
          lead(role) over (partition by session_id order by created_at, id) as next_role,
          lead(content) over (partition by session_id order by created_at, id) as answer
        from chat_messages
      )
      select session_id, content as question, answer, created_at as asked_at
      from ordered
      where role = 'user'
        and next_role = 'assistant'
        and (${conditions})
      order by created_at desc
      limit $${markers.length + 1}
    `,
    [...markers.map((marker) => `%${marker}%`), limit]
  );
  return result.rows;
}
