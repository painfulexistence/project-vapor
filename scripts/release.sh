#!/bin/bash
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}🚀 Vapor Engine Releases${NC}"

# 1. 檢查是否在 main 分支
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$CURRENT_BRANCH" != "main" ]; then
    echo -e "${RED}錯誤: 你必須在 main 分支才能進行 Release (目前在 $CURRENT_BRANCH)${NC}"
    exit 1
fi

# 2. 輸入版本號
echo -e "${GREEN}請輸入新的版本號 (例如 1.0.0):${NC}"
read VERSION

if [[ ! $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo -e "${YELLOW}提醒: 建議使用語義化版本號 (X.Y.Z)${NC}"
fi

TAG="v$VERSION"

# 3. 檢查標籤是否已存在
if git rev-parse "$TAG" >/dev/null 2>&1; then
    echo -e "${RED}錯誤: 標籤 $TAG 已經存在了。請使用新版本號，或先手動刪除舊標籤。${NC}"
    exit 1
fi

# 4. 確認
echo -e "${YELLOW}即將建立標籤 $TAG 並推送到 GitHub，這將觸發 CI Release。確定嗎？ (y/n)${NC}"
read CONFIRM

if [ "$CONFIRM" != "y" ]; then
    echo -e "已取消。"
    exit 0
fi

# 5. 執行 Tag 與 Push
echo -e "${YELLOW}正在建立標籤 $TAG...${NC}"
git tag -a "$TAG" -m "Release $TAG"

echo -e "${YELLOW}正在推送到 GitHub...${NC}"
git push origin "$TAG"

# 6. 獲取 GitHub Actions 連結
REPO_URL=$(git remote get-url origin | sed 's/\.git$//' | sed 's/git@github.com:/https:\/\/github.com\//')
ACTIONS_URL="${REPO_URL}/actions"

echo -e "${GREEN}✅ 成功！請點擊下方連結查看 Release 進度：${NC}"
echo -e "${YELLOW}${ACTIONS_URL}${NC}"
