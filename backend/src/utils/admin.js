export function getAdminEmails() {
  return (process.env.ADMIN_EMAILS || '')
    .split(',')
    .map((email) => email.trim().toLowerCase())
    .filter(Boolean);
}

export function resolveRole(email) {
  return getAdminEmails().includes(email.trim().toLowerCase()) ? 'admin' : 'user';
}
