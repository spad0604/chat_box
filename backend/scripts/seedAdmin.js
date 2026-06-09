import 'dotenv/config';
import bcrypt from 'bcryptjs';
import { connectDB, query } from '../src/config/db.js';

const adminEmail = (process.env.ADMIN_SEED_EMAIL || 'admin@fpt.edu.vn').trim().toLowerCase();
const adminPassword = process.env.ADMIN_SEED_PASSWORD || 'Admin@123456';
const adminName = process.env.ADMIN_SEED_NAME || 'Unimate Admin';

async function seedAdmin() {
  if (!process.env.DATABASE_URL) {
    console.error('Missing DATABASE_URL');
    process.exit(1);
  }

  await connectDB(process.env.DATABASE_URL);

  const passwordHash = await bcrypt.hash(adminPassword, 10);
  await query(
    `
      insert into users (name, email, password_hash, role)
      values ($1, $2, $3, 'admin')
      on conflict (email)
      do update set
        name = excluded.name,
        password_hash = excluded.password_hash,
        role = 'admin',
        updated_at = now()
    `,
    [adminName, adminEmail, passwordHash]
  );

  console.log('Admin account upserted');
  console.log(`Email: ${adminEmail}`);
  console.log(`Password: ${adminPassword}`);
  process.exit(0);
}

seedAdmin().catch((error) => {
  console.error('Failed to seed admin:', error);
  process.exit(1);
});
