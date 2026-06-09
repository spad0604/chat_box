const EMAIL_REGEX = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

function validatePassword(password) {
  if (typeof password !== 'string' || password.length < 6) {
    return 'password must be at least 6 characters';
  }
  if (password.length > 128) {
    return 'password must be at most 128 characters';
  }
  return null;
}

export function validateRegister(req, res, next) {
  const { name, email, password } = req.body ?? {};
  const errors = [];

  if (typeof name !== 'string' || !name.trim()) {
    errors.push('name is required');
  } else if (name.trim().length > 100) {
    errors.push('name must be at most 100 characters');
  }

  if (typeof email !== 'string' || !email.trim()) {
    errors.push('email is required');
  } else if (!EMAIL_REGEX.test(email.trim())) {
    errors.push('email is invalid');
  }

  const passwordError = validatePassword(password);
  if (passwordError) {
    errors.push(passwordError);
  }

  if (errors.length > 0) {
    return res.status(400).json({ message: 'Validation failed', errors });
  }

  req.validatedAuth = {
    name: name.trim(),
    email: email.trim().toLowerCase(),
    password,
  };

  next();
}

export function validateLogin(req, res, next) {
  const { email, password } = req.body ?? {};
  const errors = [];

  if (typeof email !== 'string' || !email.trim()) {
    errors.push('email is required');
  } else if (!EMAIL_REGEX.test(email.trim())) {
    errors.push('email is invalid');
  }

  const passwordError = validatePassword(password);
  if (passwordError) {
    errors.push(passwordError);
  }

  if (errors.length > 0) {
    return res.status(400).json({ message: 'Validation failed', errors });
  }

  req.validatedAuth = {
    email: email.trim().toLowerCase(),
    password,
  };

  next();
}
