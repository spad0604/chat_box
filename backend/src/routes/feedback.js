import { Router } from 'express';
import { query } from '../config/db.js';
import { adminAuth } from '../middleware/adminAuth.js';
import { jwtAuth } from '../middleware/jwtAuth.js';
import { validateFeedback } from '../middleware/validateFeedback.js';

const router = Router();

function toFeedbackResponse(feedback, includePrivate = true) {
  const response = {
    id: feedback.id,
    name: feedback.name,
    subject: feedback.subject,
    message: feedback.message,
    rating: feedback.rating,
    createdAt: feedback.created_at,
  };

  if (includePrivate) {
    response.email = feedback.email;
    response.phone = feedback.phone;
  }

  return response;
}

router.post('/', jwtAuth, validateFeedback, async (req, res) => {
  try {
    const feedback = req.validatedFeedback;
    const result = await query(
      `
        insert into feedback (user_id, name, email, phone, subject, message, rating)
        values ($1, $2, $3, $4, $5, $6, $7)
        returning id, name, email, phone, subject, message, rating, created_at
      `,
      [
        req.user.id,
        feedback.name,
        feedback.email,
        feedback.phone,
        feedback.subject,
        feedback.message,
        feedback.rating,
      ]
    );

    return res.status(201).json({
      message: 'Feedback submitted successfully',
      data: toFeedbackResponse(result.rows[0]),
    });
  } catch (error) {
    console.error('Create feedback error:', error);
    return res.status(500).json({ message: 'Internal server error' });
  }
});

router.get('/public', async (req, res) => {
  try {
    const page = Math.max(1, Number.parseInt(req.query.page, 10) || 1);
    const limit = Math.min(12, Math.max(1, Number.parseInt(req.query.limit, 10) || 3));
    const offset = (page - 1) * limit;

    const [feedbacks, total] = await Promise.all([
      query(
        `
          select id, name, subject, message, rating, created_at
          from feedback
          order by created_at desc
          limit $1 offset $2
        `,
        [limit, offset]
      ),
      query('select count(*)::int as total from feedback'),
    ]);

    return res.status(200).json({
      message: 'Public feedbacks fetched successfully',
      data: feedbacks.rows.map((feedback) => toFeedbackResponse(feedback, false)),
      pagination: {
        page,
        limit,
        total: total.rows[0].total,
        totalPages: Math.max(1, Math.ceil(total.rows[0].total / limit)),
      },
    });
  } catch (error) {
    console.error('Public feedback list error:', error);
    return res.status(500).json({ message: 'Internal server error' });
  }
});

router.get('/', adminAuth, async (_req, res) => {
  try {
    const result = await query(
      `
        select id, name, email, phone, subject, message, rating, created_at
        from feedback
        order by created_at desc
      `
    );

    return res.status(200).json({
      message: 'Feedbacks fetched successfully',
      data: result.rows.map((feedback) => toFeedbackResponse(feedback)),
    });
  } catch (error) {
    console.error('List feedback error:', error);
    return res.status(500).json({ message: 'Internal server error' });
  }
});

router.delete('/:id', adminAuth, async (req, res) => {
  try {
    const result = await query('delete from feedback where id = $1 returning id', [req.params.id]);
    const feedback = result.rows[0];

    if (!feedback) {
      return res.status(404).json({ message: 'Feedback not found' });
    }

    return res.status(200).json({
      message: 'Feedback deleted successfully',
      data: { id: feedback.id },
    });
  } catch (error) {
    if (error.code === '22P02') {
      return res.status(400).json({ message: 'Invalid feedback id' });
    }

    console.error('Delete feedback error:', error);
    return res.status(500).json({ message: 'Internal server error' });
  }
});

export default router;
