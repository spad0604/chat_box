import { query } from '../config/db.js';
import { verifyToken } from '../utils/token.js';

export async function jwtAuth(req, res, next) {
  const authHeader = req.header('Authorization');

  if (!authHeader?.startsWith('Bearer ')) {
    return res.status(401).json({ message: 'Unauthorized' });
  }

  try {
    const payload = verifyToken(authHeader.slice(7));
    const result = await query('select id, name, email, role from users where id = $1', [payload.sub]);
    const user = result.rows[0];

    if (!user) {
      return res.status(401).json({ message: 'Unauthorized' });
    }

    req.user = user;
    next();
  } catch {
    return res.status(401).json({ message: 'Invalid or expired token' });
  }
}
