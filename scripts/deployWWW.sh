#!/bin/bash
set -e

# 顏色定義
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 預設部署目的地 (可以是本地路徑如 /var/www/vapor 或遠端路徑如 user@vps:/var/www/vapor)
# 如果沒有特別指定，就只會更新本地專案根目錄下的 www 資料夾
DEPLOY_DEST="${VAPOR_WWW_PATH}"

# 解析參數
for arg in "$@"; do
    case "$arg" in
        --path=*)
            DEPLOY_DEST="${arg#*=}"
            ;;
    esac
done

LOCAL_WWW="${PROJECT_ROOT}/www"

echo -e "${BLUE}===================================================${NC}"
echo -e "${YELLOW}🚀 Project Vapor - deployWWW 部署指令碼${NC}"
echo -e "${BLUE}===================================================${NC}"
echo -e "本地建置網頁目錄 (www): ${GREEN}${LOCAL_WWW}${NC}"
echo -e "部署目標路徑:           ${GREEN}${DEPLOY_DEST}${NC}"
echo -e ""

# 1. 初始化本地 www/ 目錄
echo -e "${YELLOW}📁 正在初始化本地 www/ 目錄...${NC}"
rm -rf "${LOCAL_WWW}"
mkdir -p "${LOCAL_WWW}"

# 2. 部署 docs/ 下的內容至 www/
echo -e "${YELLOW}📁 正在同步 docs/ 目錄...${NC}"
if [ -d "${PROJECT_ROOT}/docs" ]; then
    cp -R "${PROJECT_ROOT}/docs/"* "${LOCAL_WWW}/"
    echo -e "${GREEN}✓ docs 同步成功！${NC}"
else
    echo -e "${RED}⚠️ 警告: 找不到 ${PROJECT_ROOT}/docs 目錄${NC}"
    exit 1
fi

# 3. 清除不必要的系統檔案
echo -e "${YELLOW}🧹 正在清理 .DS_Store 等系統垃圾檔案...${NC}"
find "${LOCAL_WWW}" -name ".DS_Store" -type f -delete 2>/dev/null || true
echo -e "${GREEN}✓ 本地 www/ 目錄更新完成！${NC}"

# 4. 如果指定了部署目的地，執行複製/同步
if [ -n "${DEPLOY_DEST}" ] && [ "${DEPLOY_DEST}" != "${LOCAL_WWW}" ]; then
    echo -e ""
    if [[ "$DEPLOY_DEST" == *":"* ]]; then
        # 遠端複製 (例如 user@vps:/var/www/vapor)
        echo -e "${YELLOW}🚀 偵測到遠端路徑，正在將 www/ 同步上傳至遠端 ${DEPLOY_DEST}...${NC}"
        rsync -avz --delete "${LOCAL_WWW}/" "${DEPLOY_DEST}/"
        echo -e "${GREEN}✨ 遠端部署成功！${NC}"
    else
        # 本地複製 (例如 /var/www/vapor)
        echo -e "${YELLOW}📂 正在將 www/ 同步至本地目錄 ${DEPLOY_DEST}...${NC}"
        mkdir -p "${DEPLOY_DEST}"
        rsync -av --delete "${LOCAL_WWW}/" "${DEPLOY_DEST}/"
        echo -e "${GREEN}✨ 本地部署成功！${NC}"
    fi
fi

echo -e ""
echo -e "${GREEN}✨ 部署指令碼執行完畢！${NC}"
echo -e "${BLUE}===================================================${NC}"
