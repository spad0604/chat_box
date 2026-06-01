import { ActivityIndicator, ScrollView, StyleSheet, Text, View } from "react-native";

import { fetchTopQuestions, fetchUnansweredQuestions } from "../api/client";
import { Card } from "../components/Card";
import { EmptyState } from "../components/EmptyState";
import { SectionHeader } from "../components/SectionHeader";
import { useAsyncData } from "../hooks/useAsyncData";
import { colors, spacing } from "../theme";
import { formatDate } from "../utils/format";

export function QuestionsScreen() {
  const top = useAsyncData(fetchTopQuestions, []);
  const unanswered = useAsyncData(fetchUnansweredQuestions, []);

  return (
    <View style={styles.screen}>
      <SectionHeader
        title="Phân tích câu hỏi"
        subtitle="Theo dõi chủ đề sinh viên hỏi nhiều và các câu UniMate chưa có dữ liệu để trả lời."
      />

      <View style={styles.layout}>
        <Card style={styles.panel}>
          <Text style={styles.panelTitle}>Câu hỏi nhiều nhất</Text>
          {top.loading ? <ActivityIndicator color={colors.primary} /> : null}
          {top.error ? <EmptyState title="Không tải được dữ liệu" message={top.error} /> : null}
          <ScrollView style={styles.list}>
            {top.data?.map((item, index) => (
              <View key={`${item.question}-${index}`} style={styles.questionRow}>
                <View style={styles.rank}>
                  <Text style={styles.rankText}>{index + 1}</Text>
                </View>
                <View style={styles.questionBody}>
                  <Text style={styles.questionText}>{item.question}</Text>
                  <Text style={styles.meta}>Lần cuối: {formatDate(item.last_asked_at)}</Text>
                </View>
                <Text style={styles.count}>{item.count}</Text>
              </View>
            ))}
            {top.data?.length === 0 && !top.loading ? <EmptyState title="Chưa có câu hỏi" /> : null}
          </ScrollView>
        </Card>

        <Card style={styles.panel}>
          <Text style={styles.panelTitle}>Câu hỏi chưa trả lời được</Text>
          {unanswered.loading ? <ActivityIndicator color={colors.primary} /> : null}
          {unanswered.error ? <EmptyState title="Không tải được dữ liệu" message={unanswered.error} /> : null}
          <ScrollView style={styles.list}>
            {unanswered.data?.map((item, index) => (
              <View key={`${item.session_id}-${item.asked_at}-${index}`} style={styles.unansweredItem}>
                <Text style={styles.questionText}>{item.question}</Text>
                <Text style={styles.answerText}>{item.answer}</Text>
                <Text style={styles.meta}>
                  {formatDate(item.asked_at)} · {item.session_id}
                </Text>
              </View>
            ))}
            {unanswered.data?.length === 0 && !unanswered.loading ? (
              <EmptyState title="Chưa có câu chưa trả lời được" />
            ) : null}
          </ScrollView>
        </Card>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: {
    gap: spacing.lg
  },
  layout: {
    flexDirection: "row",
    flexWrap: "wrap",
    gap: spacing.md
  },
  panel: {
    flexBasis: 420,
    flexGrow: 1,
    maxHeight: 690
  },
  panelTitle: {
    color: colors.text,
    fontSize: 17,
    fontWeight: "900",
    marginBottom: spacing.md
  },
  list: {
    maxHeight: 600
  },
  questionRow: {
    alignItems: "center",
    borderBottomColor: colors.border,
    borderBottomWidth: 1,
    flexDirection: "row",
    gap: spacing.md,
    paddingVertical: spacing.md
  },
  rank: {
    alignItems: "center",
    backgroundColor: colors.primarySoft,
    borderRadius: 18,
    height: 36,
    justifyContent: "center",
    width: 36
  },
  rankText: {
    color: colors.primary,
    fontWeight: "900"
  },
  questionBody: {
    flex: 1,
    gap: spacing.xs
  },
  questionText: {
    color: colors.text,
    fontSize: 14,
    fontWeight: "800",
    lineHeight: 20
  },
  answerText: {
    color: colors.muted,
    fontSize: 13,
    lineHeight: 19,
    marginTop: spacing.sm
  },
  meta: {
    color: colors.muted,
    fontSize: 12
  },
  count: {
    color: colors.primary,
    fontSize: 20,
    fontWeight: "900",
    minWidth: 34,
    textAlign: "right"
  },
  unansweredItem: {
    borderBottomColor: colors.border,
    borderBottomWidth: 1,
    paddingVertical: spacing.md
  }
});

