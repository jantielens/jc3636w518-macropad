#!/bin/bash
#
# Build HTML pages by assembling shared components with page content
# Called by minify-web-assets.sh before minification
#

set -e

# Resolve script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEB_DIR="$SCRIPT_DIR/../src/app/web"
SHARED_DIR="$WEB_DIR/shared"
PAGES_DIR="$WEB_DIR/pages"

echo "Building HTML pages from components..."

# Function to build a page
# Args: page_name, page_title, active_tab
build_page() {
    local page_name="$1"
    local page_title="$2"
    local active_index=""
    local active_network=""
    local active_update=""
    
    # Set active tab
    case "$page_name" in
        index)
            active_index="active"
            ;;
        network)
            active_network="active"
            ;;
        update)
            active_update="active"
            ;;
    esac
    
    echo "  - Building $page_name.html..."
    
    # Assemble the page
    {
        # Header
        sed -e "s/{{PAGE_TITLE}}/$page_title/g" "$SHARED_DIR/header.html"
        
        # Navigation with active tab
        sed -e "s/{{CURRENT_PAGE}}/$page_name/g" \
            -e "s/{{ACTIVE_INDEX}}/$active_index/g" \
            -e "s/{{ACTIVE_NETWORK}}/$active_network/g" \
            -e "s/{{ACTIVE_UPDATE}}/$active_update/g" \
            "$SHARED_DIR/nav.html"
        
        # Page content
        cat "$PAGES_DIR/${page_name}-content.html"
        
        # Footer
        cat "$SHARED_DIR/footer.html"
        
        # Health widget
        cat "$SHARED_DIR/health-widget.html"
        
        # Reboot overlays
        cat "$SHARED_DIR/reboot-overlay.html"
        
    } > "$WEB_DIR/${page_name}.html"
}

# Build all pages
build_page "index" "Macropad"
build_page "network" "Network"
build_page "update" "Update"

echo "✓ HTML pages built successfully"
echo "  Generated: index.html, network.html, update.html"
