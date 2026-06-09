import { query } from '../config/db.js';
import { verifyToken } from '../utils/token.js';

export async function adminAuth(req, res, next) {
  const adminKey = process.env.ADMIN_API_KEY;
  const providedKey = req.header('x-admin-key');

  if (adminKey && providedKey && providedKey === adminKey) {
    return next();
  }

  const authHeader = req.header('Authorization');
  if (!authHeader?.startsWith('Bearer ')) {
    return res.status(401).json({ message: 'Unauthorized' });
  }

  try {
    const payload = verifyToken(authHeader.slice(7));
    const result = await query('select id, name, email, role from users where id = $1', [payload.sub]);
    const user = result.rows[0];

    if (!user || user.role !== 'admin') {
      return res.status(403).json({ message: 'Admin access required' });
    }

    req.user = user;
    next();
  } catch {
    return res.status(401).json({ message: 'Unauthorized' });
  }
}
