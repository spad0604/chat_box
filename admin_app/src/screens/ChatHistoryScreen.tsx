import { useEffect, useState } from "react";
import { ActivityIndicator, Pressable, ScrollView, StyleSheet, Text, View } from "react-native";

import { fetchHistory, fetchSessions } from "../api/client";
import type { ChatSession } from "../api/types";
import { Card } from "../components/Card";
import { EmptyState } from "../components/EmptyState";
import { SectionHeader } from "../components/SectionHeader";
import { useAsyncData } from "../hooks/useAsyncData";
import { colors, radius, spacing } from "../theme";
import { compactText, formatDate } from "../utils/format";

export function ChatHistoryScreen() {
  const [selected, setSelected] = useState<ChatSession | null>(null);
  const sessions = useAsyncData(fetchSessions, []);
  const history = useAsyncData(
    () => (selected ? fetchHistory(selected.session_id) : Promise.resolve([])),
    [selected?.session_id]
  );

  useEffect(() => {
    if (!selected && sessions.data?.[0]) {
      setSelected(sessions.data[0]);
    }
  }, [selected, sessions.data]);

  return (
    <View style={styles.screen}>
      <SectionHeader title="Lịch sử chat" subtitle="Tra cứu phiên hỏi đáp và nội dung từng cuộc trò chuyện." />

      <View style={styles.layout}>
        <Card style={styles.sessionsPanel}>
          <Text style={styles.panelTitle}>Phiên gần đây</Text>
          {sessions.loading ? <ActivityIndicator color={colors.primary} /> : null}
          {sessions.error ? <EmptyState title="Không tải được phiên chat" message={sessions.error} /> : null}
          <ScrollView style={styles.sessionList}>
            {sessions.data?.map((session) => (
              <Pressable
                key={session.session_id}
                onPress={() => setSelected(session)}
                style={[styles.sessionItem, selected?.session_id === session.session_id && styles.sessionActive]}
              >
                <Text style={styles.sessionTitle}>{compactText(session.last_message, 78)}</Text>
                <Text style={styles.sessionMeta}>{formatDate(session.updated_at)}</Text>
              </Pressable>
            ))}
          </ScrollView>
        </Card>

        <Card style={styles.chatPanel}>
          <Text style={styles.panelTitle}>Nội dung phiên</Text>
          {history.loading ? <ActivityIndicator color={colors.primary} /> : null}
          {history.error ? <EmptyState title="Không tải được lịch sử" message={history.error} /> : null}
          <ScrollView style={styles.messages}>
            {history.data?.length === 0 && !history.loading ? <EmptyState title="Chọn một phiên chat" /> : null}
            {history.data?.map((message, index) => {
              const isUser = message.role === "user";
              return (
                <View key={`${message.created_at}-${index}`} style={[styles.message, isUser && styles.userMessage]}>
                  <Text style={[styles.messageRole, isUser && styles.userRole]}>
                    {isUser ? "Sinh viên" : "UniMate"}
                  </Text>
                  <Text style={styles.messageText}>{message.content}</Text>
                  <Text style={styles.messageTime}>{formatDate(message.created_at)}</Text>
                </View>
              );
            })}
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
  sessionsPanel: {
    flexBasis: 320,
    flexGrow: 1,
    maxHeight: 680
  },
  chatPanel: {
    flexBasis: 520,
    flexGrow: 2,
    maxHeight: 680
  },
  panelTitle: {
    color: colors.text,
    fontSize: 17,
    fontWeight: "900",
    marginBottom: spacing.md
  },
  sessionList: {
    maxHeight: 590
  },
  sessionItem: {
    borderBottomColor: colors.border,
    borderBottomWidth: 1,
    gap: spacing.xs,
    paddingVertical: spacing.md
  },
  sessionActive: {
    backgroundColor: colors.primarySoft,
    borderRadius: radius.sm,
    paddingHorizontal: spacing.sm
  },
  sessionTitle: {
    color: colors.text,
    fontSize: 14,
    fontWeight: "800",
    lineHeight: 19
  },
  sessionMeta: {
    color: colors.muted,
    fontSize: 12
  },
  messages: {
    maxHeight: 590
  },
  message: {
    alignSelf: "flex-start",
    backgroundColor: colors.surfaceMuted,
    borderRadius: radius.md,
    marginBottom: spacing.md,
    maxWidth: "86%",
    padding: spacing.md
  },
  userMessage: {
    alignSelf: "flex-end",
    backgroundColor: colors.primarySoft
  },
  messageRole: {
    color: colors.blue,
    fontSize: 12,
    fontWeight: "900",
    marginBottom: spacing.xs
  },
  userRole: {
    color: colors.primary
  },
  messageText: {
    color: colors.text,
    fontSize: 14,
    lineHeight: 21
  },
  messageTime: {
    color: colors.muted,
    fontSize: 11,
    marginTop: spacing.sm
  }
});
