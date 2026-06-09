import { Router } from 'express';
import bcrypt from 'bcryptjs';
import { query } from '../config/db.js';
import { jwtAuth } from '../middleware/jwtAuth.js';
import { validateLogin, validateRegister } from '../middleware/validateAuth.js';
import { signToken } from '../utils/token.js';
import { resolveRole } from '../utils/admin.js';

const router = Router();

function toUserResponse(user) {
  return {
    id: user.id,
    name: user.name,
    email: user.email,
    role: user.role ?? 'user',
    createdAt: user.created_at,
  };
}

async function findUserByEmail(email) {
  const result = await query('select * from users where email = $1', [email]);
  return result.rows[0] ?? null;
}

async function findUserById(id) {
  const result = await query('select id, name, email, role, created_at from users where id = $1', [id]);
  return result.rows[0] ?? null;
}

async function syncAdminRole(user) {
  const role = resolveRole(user.email);
  if (role === 'admin' && user.role !== 'admin') {
    const result = await query(
      "update users set role = 'admin', updated_at = now() where id = $1 returning id, name, email, role, created_at",
      [user.id]
    );
    return result.rows[0];
  }
  return user;
}

router.post('/register', validateRegister, async (req, res) => {
  try {
    const { name, email, password } = req.validatedAuth;

    const existingUser = await findUserByEmail(email);
    if (existingUser) {
      return res.status(409).json({ message: 'Email already registered' });
    }

    const passwordHash = await bcrypt.hash(password, 10);
    const result = await query(
      `
        insert into users (name, email, password_hash, role)
        values ($1, $2, $3, $4)
        returning id, name, email, role, created_at
      `,
      [name, email, passwordHash, resolveRole(email)]
    );
    const user = result.rows[0];
    const token = signToken(user.id);

    return res.status(201).json({
      message: 'Registration successful',
      data: {
        token,
        user: toUserResponse(user),
      },
    });
  } catch (error) {
    console.error('Register error:', error);
    return res.status(500).json({ message: 'Internal server error' });
  }
});

router.post('/login', validateLogin, async (req, res) => {
  try {
    const { email, password } = req.validatedAuth;

    const user = await findUserByEmail(email);
    if (!user) {
      return res.status(401).json({ message: 'Invalid email or password' });
    }

    const isValidPassword = await bcrypt.compare(password, user.password_hash);
    if (!isValidPassword) {
      return res.status(401).json({ message: 'Invalid email or password' });
    }

    const syncedUser = await syncAdminRole(user);
    const token = signToken(syncedUser.id);

    return res.status(200).json({
      message: 'Login successful',
      data: {
        token,
        user: toUserResponse(syncedUser),
      },
    });
  } catch (error) {
    console.error('Login error:', error);
    return res.status(500).json({ message: 'Internal server error' });
  }
});

router.get('/me', jwtAuth, async (req, res) => {
  try {
    const user = await findUserById(req.user.id);
    if (!user) {
      return res.status(401).json({ message: 'Unauthorized' });
    }

    const syncedUser = await syncAdminRole(user);

    return res.status(200).json({
      message: 'User fetched successfully',
      data: toUserResponse(syncedUser),
    });
  } catch (error) {
    console.error('Me error:', error);
    return res.status(500).json({ message: 'Internal server error' });
  }
});

export default router;
