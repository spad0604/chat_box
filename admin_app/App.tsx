import { Ionicons } from "@expo/vector-icons";
import { useMemo, useState } from "react";
import { Platform, Pressable, SafeAreaView, ScrollView, StyleSheet, Text, View } from "react-native";
import { StatusBar } from "expo-status-bar";

import { API_BASE_URL } from "./src/api/client";
import { DashboardScreen } from "./src/screens/DashboardScreen";
import { ChatHistoryScreen } from "./src/screens/ChatHistoryScreen";
import { DocumentsScreen } from "./src/screens/DocumentsScreen";
import { QuestionsScreen } from "./src/screens/QuestionsScreen";
import { colors, radius, spacing } from "./src/theme";

type TabKey = "dashboard" | "history" | "questions" | "documents";

const tabs: Array<{
  key: TabKey;
  label: string;
  icon: keyof typeof Ionicons.glyphMap;
}> = [
  { key: "dashboard", label: "Tổng quan", icon: "analytics-outline" },
  { key: "history", label: "Lịch sử chat", icon: "chatbubbles-outline" },
  { key: "questions", label: "Câu hỏi", icon: "help-circle-outline" },
  { key: "documents", label: "Tài liệu", icon: "document-text-outline" }
];

export default function App() {
  const [activeTab, setActiveTab] = useState<TabKey>("dashboard");

  const content = useMemo(() => {
    if (activeTab === "history") return <ChatHistoryScreen />;
    if (activeTab === "questions") return <QuestionsScreen />;
    if (activeTab === "documents") return <DocumentsScreen />;
    return <DashboardScreen />;
  }, [activeTab]);

  return (
    <SafeAreaView style={styles.safe}>
      <StatusBar style="dark" />
      <View style={styles.root}>
        <View style={styles.sidebar}>
          <View style={styles.brandRow}>
            <View style={styles.brandMark}>
              <Text style={styles.brandMarkText}>F</Text>
            </View>
            <View>
              <Text style={styles.brandTitle}>UniMate</Text>
              <Text style={styles.brandSub}>Admin Console</Text>
            </View>
          </View>

          <View style={styles.nav}>
            {tabs.map((tab) => {
              const active = activeTab === tab.key;
              return (
                <Pressable
                  key={tab.key}
                  onPress={() => setActiveTab(tab.key)}
                  style={[styles.navItem, active && styles.navActive]}
                >
                  <Ionicons name={tab.icon} size={20} color={active ? colors.primary : colors.muted} />
                  <Text style={[styles.navText, active && styles.navTextActive]}>{tab.label}</Text>
                </Pressable>
              );
            })}
          </View>

          <View style={styles.serverBox}>
            <Text style={styles.serverLabel}>API</Text>
            <Text style={styles.serverText}>{API_BASE_URL.replace("/api/v1", "")}</Text>
          </View>
        </View>

        <ScrollView style={styles.content} contentContainerStyle={styles.contentInner}>
          {content}
        </ScrollView>
      </View>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  safe: {
    backgroundColor: colors.background,
    flex: 1
  },
  root: {
    flex: 1,
    flexDirection: Platform.OS === "web" ? "row" : "column"
  },
  sidebar: {
    backgroundColor: colors.surface,
    borderColor: colors.border,
    borderRightWidth: Platform.OS === "web" ? 1 : 0,
    borderBottomWidth: Platform.OS === "web" ? 0 : 1,
    gap: spacing.lg,
    padding: spacing.lg,
    width: Platform.OS === "web" ? 280 : "100%"
  },
  brandRow: {
    alignItems: "center",
    flexDirection: "row",
    gap: spacing.md
  },
  brandMark: {
    alignItems: "center",
    backgroundColor: colors.primary,
    borderRadius: 18,
    height: 44,
    justifyContent: "center",
    width: 44
  },
  brandMarkText: {
    color: "#ffffff",
    fontSize: 18,
    fontWeight: "900"
  },
  brandTitle: {
    color: colors.text,
    fontSize: 21,
    fontWeight: "900"
  },
  brandSub: {
    color: colors.muted,
    fontSize: 12,
    fontWeight: "700"
  },
  nav: {
    flexDirection: Platform.OS === "web" ? "column" : "row",
    flexWrap: "wrap",
    gap: spacing.sm
  },
  navItem: {
    alignItems: "center",
    borderRadius: radius.md,
    flexDirection: "row",
    gap: spacing.sm,
    minHeight: 44,
    paddingHorizontal: spacing.md
  },
  navActive: {
    backgroundColor: colors.primarySoft
  },
  navText: {
    color: colors.muted,
    fontSize: 14,
    fontWeight: "800"
  },
  navTextActive: {
    color: colors.primary
  },
  serverBox: {
    backgroundColor: colors.surfaceMuted,
    borderColor: colors.border,
    borderRadius: radius.md,
    borderWidth: 1,
    gap: spacing.xs,
    marginTop: "auto",
    padding: spacing.md
  },
  serverLabel: {
    color: colors.muted,
    fontSize: 11,
    fontWeight: "900"
  },
  serverText: {
    color: colors.text,
    fontSize: 12,
    fontWeight: "700"
  },
  content: {
    flex: 1
  },
  contentInner: {
    padding: spacing.xl
  }
});

