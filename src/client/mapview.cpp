/*
 * Copyright (c) 2010-2020 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "mapview.h"

#include "animatedtext.h"
#include "creature.h"
#include "game.h"
#include "lightview.h"
#include "map.h"
#include "missile.h"
#include "shadermanager.h"
#include "statictext.h"
#include "tile.h"

#include <framework/core/application.h>
#include <framework/core/eventdispatcher.h>
#include <framework/core/resourcemanager.h>
#include <framework/graphics/framebuffermanager.h>
#include <framework/graphics/graphics.h>
#include <framework/graphics/image.h>
#include <framework/graphics/drawpool.h>

enum {
    // 3840x2160 => 1080p optimized
    // 2560x1440 => 720p optimized
    // 1728x972 => 480p optimized

    NEAR_VIEW_AREA = 32 * 32,
    MID_VIEW_AREA = 64 * 64,
    FAR_VIEW_AREA = 128 * 128
};

MapView::MapView()
{
    m_optimizedSize = Size(g_map.getAwareRange().horizontal(), g_map.getAwareRange().vertical()) * Otc::TILE_PIXELS;

    m_pools.map = g_drawPool.createPoolF(PoolType::MAP);
    m_pools.creatureInformation = g_drawPool.createPool(PoolType::CREATURE_INFORMATION);
    m_pools.text = g_drawPool.createPool(PoolType::TEXT);

    m_pools.map->onBeforeDraw([&]() {
        const Position cameraPosition = getCameraPosition();

        float fadeOpacity = 1.0f;
        if(!m_shaderSwitchDone && m_fadeOutTime > 0) {
            fadeOpacity = 1.0f - (m_fadeTimer.timeElapsed() / m_fadeOutTime);
            if(fadeOpacity < 0.0f) {
                m_shader = m_nextShader;
                m_nextShader = nullptr;
                m_shaderSwitchDone = true;
                m_fadeTimer.restart();
            }
        }

        if(m_shaderSwitchDone && m_shader && m_fadeInTime > 0)
            fadeOpacity = std::min<float>(m_fadeTimer.timeElapsed() / m_fadeInTime, 1.0f);

        if(m_shader && g_painter->hasShaders() && g_graphics.shouldUseShaders()) {
            Rect framebufferRect = Rect(0, 0, m_drawDimension * m_tileSize);
            const Point center = m_rectCache.srcRect.center();
            const Point globalCoord = Point(cameraPosition.x - m_drawDimension.width() / 2, -(cameraPosition.y - m_drawDimension.height() / 2)) * m_tileSize;
            m_shader->bind();
            m_shader->setUniformValue(ShaderManager::MAP_CENTER_COORD, center.x / (float)m_rectDimension.width(), 1.0f - center.y / (float)m_rectDimension.height());
            m_shader->setUniformValue(ShaderManager::MAP_GLOBAL_COORD, globalCoord.x / (float)m_rectDimension.height(), globalCoord.y / (float)m_rectDimension.height());
            m_shader->setUniformValue(ShaderManager::MAP_ZOOM, m_scaleFactor);

            Point last = transformPositionTo2D(cameraPosition, m_shader->getPosition());
            //Reverse vertical axis.
            last.y = -last.y;

            m_shader->setUniformValue(ShaderManager::MAP_WALKOFFSET, last.x / (float)m_rectDimension.width(), last.y / (float)m_rectDimension.height());

            g_painter->setShaderProgram(m_shader);
        }

        g_painter->setOpacity(fadeOpacity);
    });

    m_pools.map->onAfterDraw([&]() {
        g_painter->resetShaderProgram();
        g_painter->resetOpacity();
    });

    m_shader = g_shaders.getDefaultMapShader();

    setVisibleDimension(Size(15, 11));
}

MapView::~MapView()
{
#ifndef NDEBUG
    assert(!g_app.isTerminated());
#endif
}

void MapView::draw(const Rect& rect)
{
    // update visible tiles cache when needed
    if(m_mustUpdateVisibleTilesCache)
        updateVisibleTilesCache();

    if(m_rectCache.rect != rect) {
        m_rectCache.rect = rect;
        m_rectCache.srcRect = calcFramebufferSource(rect.size());
        m_rectCache.drawOffset = m_rectCache.srcRect.topLeft();
        m_rectCache.horizontalStretchFactor = rect.width() / static_cast<float>(m_rectCache.srcRect.width());
        m_rectCache.verticalStretchFactor = rect.height() / static_cast<float>(m_rectCache.srcRect.height());
    }

    drawFloor();

    // this could happen if the player position is not known yet
    if(!getCameraPosition().isValid()) {
        return;
    }

    drawCreatureInformation();
    if(m_drawLights) m_lightView->draw(rect, m_rectCache.srcRect);
    drawText();
}

void MapView::drawFloor()
{
    g_drawPool.use(m_pools.map, m_rectCache.rect, m_rectCache.srcRect);
    {
        const Position cameraPosition = getCameraPosition();
        const auto& lightView = m_drawLights ? m_lightView.get() : nullptr;

        g_drawPool.addFilledRect(m_rectDimension, Color::black);
        for(int_fast8_t z = m_floorMax; z >= m_floorMin; --z) {
            if(lightView) {
                const int8 nextFloor = z - 1;
                if(nextFloor >= m_floorMin) {
                    lightView->setFloor(nextFloor);
                    for(const auto& tile : m_cachedVisibleTiles[nextFloor].grounds) {
                        const auto& ground = tile->getGround();
                        if(ground && !ground->isTranslucent()) {
                            auto pos2D = transformPositionTo2D(tile->getPosition(), cameraPosition);
                            if(ground->isTopGround()) {
                                const auto currentPos = tile->getPosition();
                                for(const auto& pos : currentPos.translatedToDirections({ Otc::South, Otc::East })) {
                                    const auto& nextDownTile = g_map.getTile(pos);
                                    if(nextDownTile && nextDownTile->hasGround() && !nextDownTile->isTopGround()) {
                                        lightView->setShade(pos2D);
                                        break;
                                    }
                                }

                                pos2D -= m_tileSize;
                            }

                            lightView->setShade(pos2D);
                        }
                    }
                }
            }

            onFloorDrawingStart(z);

            if(lightView) lightView->setFloor(z);

            const auto& map = m_cachedVisibleTiles[z];

            g_drawPool.startPosition();
            {
                for(const auto& tile : map.grounds)
                    tile->drawGround(this, transformPositionTo2D(tile->getPosition(), cameraPosition), m_scaleFactor, Otc::FUpdateAll, lightView);

                for(const auto& tile : map.borders)
                    tile->drawGroundBorder(this, transformPositionTo2D(tile->getPosition(), cameraPosition), m_scaleFactor, Otc::FUpdateAll, lightView);

                for(const auto& tile : map.bottomTops)
                    tile->draw(this, transformPositionTo2D(tile->getPosition(), cameraPosition), m_scaleFactor, Otc::FUpdateAll, lightView);
            }

            g_drawPool.startPosition();
            {
                for(const MissilePtr& missile : g_map.getFloorMissiles(z))
                    missile->draw(transformPositionTo2D(missile->getPosition(), cameraPosition), m_scaleFactor, Otc::FUpdateAll, lightView);
            }

            if(m_shadowFloorIntensity > 0 && z == cameraPosition.z + 1) {
                g_drawPool.addFilledRect(m_rectDimension, Color::black);
                g_drawPool.setOpacity(m_shadowFloorIntensity, g_drawPool.size());
            }

            onFloorDrawingEnd(z);
        }

        if(m_crosshairTexture && m_mousePosition.isValid()) {
            const Point& point = transformPositionTo2D(m_mousePosition, cameraPosition);
            const auto crosshairRect = Rect(point, m_tileSize, m_tileSize);
            g_drawPool.addTexturedRect(crosshairRect, m_crosshairTexture);
        }
    }
}

void MapView::drawCreatureInformation()
{
    if(!m_drawNames && !m_drawHealthBars && !m_drawManaBar) return;

    g_drawPool.use(m_pools.creatureInformation);
    const Position cameraPosition = getCameraPosition();

    uint32_t flags = 0;
    if(m_drawNames) { flags = Otc::DrawNames; }
    if(m_drawHealthBars) { flags |= Otc::DrawBars; }
    if(m_drawManaBar) { flags |= Otc::DrawManaBar; }

    for(const auto& creature : m_visibleCreatures) {
        creature->drawInformation(m_rectCache.rect,
                                  transformPositionTo2D(creature->getPosition(), cameraPosition),
                                  m_scaleFactor, m_rectCache.drawOffset,
                                  m_rectCache.horizontalStretchFactor, m_rectCache.verticalStretchFactor, flags);
    }
}

void MapView::drawText()
{
    if(!m_drawTexts || g_map.getStaticTexts().empty() && g_map.getAnimatedTexts().empty()) return;

    g_drawPool.use(m_pools.text);
    const Position cameraPosition = getCameraPosition();
    for(const StaticTextPtr& staticText : g_map.getStaticTexts()) {
        if(staticText->getMessageMode() == Otc::MessageNone) continue;

        const Position pos = staticText->getPosition();

        if(pos.z != cameraPosition.z)
            continue;

        Point p = transformPositionTo2D(pos, cameraPosition) - m_rectCache.drawOffset;
        p.x *= m_rectCache.horizontalStretchFactor;
        p.y *= m_rectCache.verticalStretchFactor;
        p += m_rectCache.rect.topLeft();
        staticText->drawText(p, m_rectCache.rect);
    }

    for(const AnimatedTextPtr& animatedText : g_map.getAnimatedTexts()) {
        const Position pos = animatedText->getPosition();

        if(pos.z != cameraPosition.z)
            continue;

        Point p = transformPositionTo2D(pos, cameraPosition) - m_rectCache.drawOffset;
        p.x *= m_rectCache.horizontalStretchFactor;
        p.y *= m_rectCache.verticalStretchFactor;
        p += m_rectCache.rect.topLeft();

        animatedText->drawText(p, m_rectCache.rect);
    }
}

void MapView::updateVisibleTilesCache()
{
    // there is no tile to render on invalid positions
    const Position cameraPosition = getCameraPosition();
    if(!cameraPosition.isValid())
        return;

    m_mustUpdateVisibleTilesCache = false;

    if(m_lastCameraPosition != cameraPosition) {
        if(m_mousePosition.isValid()) {
            if(cameraPosition.z == m_lastCameraPosition.z) {
                m_mousePosition = m_mousePosition.translatedToDirection(m_lastCameraPosition.getDirectionFromPosition(cameraPosition));
            } else {
                m_mousePosition.z += cameraPosition.z - m_lastCameraPosition.z;
            }

            onMouseMove(m_mousePosition, true);
        }

        onPositionChange(cameraPosition, m_lastCameraPosition);

        if(m_lastCameraPosition.z != cameraPosition.z) {
            onFloorChange(cameraPosition.z, m_lastCameraPosition.z);
        }
    }

    const uint8 cachedFirstVisibleFloor = calcFirstVisibleFloor();
    uint8 cachedLastVisibleFloor = calcLastVisibleFloor();

    assert(cachedFirstVisibleFloor >= 0 && cachedLastVisibleFloor >= 0 &&
           cachedFirstVisibleFloor <= Otc::MAX_Z && cachedLastVisibleFloor <= Otc::MAX_Z);

    if(cachedLastVisibleFloor < cachedFirstVisibleFloor)
        cachedLastVisibleFloor = cachedFirstVisibleFloor;

    m_lastCameraPosition = cameraPosition;
    m_cachedFirstVisibleFloor = cachedFirstVisibleFloor;
    m_cachedLastVisibleFloor = cachedLastVisibleFloor;

    // clear current visible tiles cache
    do {
        m_cachedVisibleTiles[m_floorMin].clear();
    } while(++m_floorMin <= m_floorMax);

    m_floorMin = m_floorMax = cameraPosition.z;

    if(m_mustUpdateVisibleCreaturesCache) {
        m_visibleCreatures.clear();
    }

    // cache visible tiles in draw order
    // draw from last floor (the lower) to first floor (the higher)
    const uint32 numDiagonals = m_drawDimension.width() + m_drawDimension.height() - 1;
    for(int_fast32_t iz = m_cachedLastVisibleFloor; iz >= m_cachedFirstVisibleFloor; --iz) {
        auto& floor = m_cachedVisibleTiles[iz];

        // loop through / diagonals beginning at top left and going to top right
        for(uint_fast32_t diagonal = 0; diagonal < numDiagonals; ++diagonal) {
            // loop current diagonal tiles
            const uint32 advance = std::max<uint32>(diagonal - m_drawDimension.height(), 0);
            for(int iy = diagonal - advance, ix = advance; iy >= 0 && ix < m_drawDimension.width(); --iy, ++ix) {
                // position on current floor
                //TODO: check position limits
                Position tilePos = cameraPosition.translated(ix - m_virtualCenterOffset.x, iy - m_virtualCenterOffset.y);
                // adjust tilePos to the wanted floor
                tilePos.coveredUp(cameraPosition.z - iz);
                if(const TilePtr& tile = g_map.getTile(tilePos)) {
                    // skip tiles that have nothing
                    if(!tile->isDrawable())
                        continue;

                    if(m_mustUpdateVisibleCreaturesCache) {
                        const auto& tileCreatures = tile->getCreatures();
                        if(isInRange(tilePos) && !tileCreatures.empty()) {
                            m_visibleCreatures.insert(m_visibleCreatures.end(), tileCreatures.rbegin(), tileCreatures.rend());
                        }
                    }

                    // skip tiles that are completely behind another tile
                    if(tile->isCompletelyCovered(m_cachedFirstVisibleFloor) && !tile->hasLight())
                        continue;

                    if(tile->hasGround())
                        floor.grounds.push_back(tile);

                    if(tile->hasGroundBorderToDraw())
                        floor.borders.push_back(tile);

                    if(tile->hasBottomOrTopToDraw())
                        floor.bottomTops.push_back(tile);

                    tile->onAddVisibleTileList(this);

                    if(iz < m_floorMin)
                        m_floorMin = iz;
                    else if(iz > m_floorMax)
                        m_floorMax = iz;
                }
            }
        }
    }

    m_mustUpdateVisibleCreaturesCache = false;
    m_mustUpdateVisibleTilesCache = false;
}

void MapView::updateGeometry(const Size& visibleDimension, const Size& optimizedSize)
{
    const uint8 tileSize = Otc::TILE_PIXELS * (static_cast<float>(m_renderScale) / 100);
    const Size drawDimension = visibleDimension + Size(3),
        bufferSize = drawDimension * tileSize;

    if(bufferSize.width() > g_graphics.getMaxTextureSize() || bufferSize.height() > g_graphics.getMaxTextureSize()) {
        g_logger.traceError("reached max zoom out");
        return;
    }

    const Point virtualCenterOffset = (drawDimension / 2 - Size(1)).toPoint(),
        visibleCenterOffset = virtualCenterOffset;

    ViewMode viewMode = m_viewMode;
    if(m_autoViewMode) {
        if(tileSize >= Otc::TILE_PIXELS && visibleDimension.area() <= NEAR_VIEW_AREA)
            viewMode = NEAR_VIEW;
        else if(tileSize >= 16 && visibleDimension.area() <= MID_VIEW_AREA)
            viewMode = MID_VIEW;
        else if(tileSize >= 8 && visibleDimension.area() <= FAR_VIEW_AREA)
            viewMode = FAR_VIEW;
        else
            viewMode = HUGE_VIEW;

        m_multifloor = viewMode < FAR_VIEW;
    }

    // draw actually more than what is needed to avoid massive recalculations on huge views
    /* if(viewMode >= HUGE_VIEW) {
        Size oldDimension = drawDimension;
        drawDimension = (m_framebuffer->getSize() / tileSize);
        virtualCenterOffset += (drawDimension - oldDimension).toPoint() / 2;
    }*/

    m_viewMode = viewMode;
    m_visibleDimension = visibleDimension;
    m_drawDimension = drawDimension;
    m_tileSize = tileSize;
    m_virtualCenterOffset = virtualCenterOffset;
    m_visibleCenterOffset = visibleCenterOffset;
    m_optimizedSize = optimizedSize;

    m_rectDimension = Rect(0, 0, bufferSize);

    m_scaleFactor = m_tileSize / static_cast<float>(Otc::TILE_PIXELS);

    m_pools.map->resize(bufferSize);
    if(m_drawLights) m_lightView->resize();

    m_awareRange.left = std::min<uint16>(g_map.getAwareRange().left, (m_drawDimension.width() / 2) - 1);
    m_awareRange.top = std::min<uint16>(g_map.getAwareRange().top, (m_drawDimension.height() / 2) - 1);
    m_awareRange.bottom = m_awareRange.top + 1;
    m_awareRange.right = m_awareRange.left + 1;
    m_rectCache.rect = Rect();

    updateViewportDirectionCache();
    requestVisibleTilesCacheUpdate();
}

void MapView::onCameraMove(const Point& /*offset*/)
{
    m_rectCache.rect = Rect();

    if(isFollowingCreature()) {
        if(m_followingCreature->isWalking()) {
            m_viewport = m_viewPortDirection[m_followingCreature->getDirection()];
        } else {
            m_viewport = m_viewPortDirection[Otc::InvalidDirection];
        }
    }
}

void MapView::onGlobalLightChange(const Light&)
{
    updateLight();
}

void MapView::updateLight()
{
    if(!m_drawLights) return;

    const auto cameraPosition = getCameraPosition();

    Light ambientLight = cameraPosition.z > Otc::SEA_FLOOR ? Light() : g_map.getLight();
    ambientLight.intensity = std::max<uint8>(m_minimumAmbientLight * 255, ambientLight.intensity);

    m_lightView->setGlobalLight(ambientLight);
}

void MapView::onFloorChange(const uint8 /*floor*/, const uint8 /*previousFloor*/)
{
    m_mustUpdateVisibleCreaturesCache = true;
    updateLight();
}

void MapView::onFloorDrawingStart(const uint8 /*floor*/) {}
void MapView::onFloorDrawingEnd(const uint8 /*floor*/) {}

void MapView::onTileUpdate(const Position&, const ThingPtr& thing, const Otc::Operation)
{
    if(thing && thing->isCreature())
        m_mustUpdateVisibleCreaturesCache = true;

    requestVisibleTilesCacheUpdate();
}

void MapView::onPositionChange(const Position& /*newPos*/, const Position& /*oldPos*/) {}

// isVirtualMove is when the mouse is stopped, but the camera moves,
// so the onMouseMove event is triggered by sending the new tile position that the mouse is in.
void MapView::onMouseMove(const Position& mousePos, const bool /*isVirtualMove*/)
{
    { // Highlight Target System
        if(m_lastHighlightTile) {
            m_lastHighlightTile->unselect();
            m_lastHighlightTile = nullptr;
        }

        if(m_drawHighlightTarget) {
            if(m_lastHighlightTile = m_shiftPressed ? getTopTile(mousePos) : g_map.getTile(mousePos))
                m_lastHighlightTile->select(m_shiftPressed);
        }
    }
}

void MapView::onKeyRelease(const InputEvent& inputEvent)
{
    const bool shiftPressed = inputEvent.keyboardModifiers == Fw::KeyboardShiftModifier;
    if(shiftPressed != m_shiftPressed) {
        m_shiftPressed = shiftPressed;
        onMouseMove(m_mousePosition);
    }
}

void MapView::onMapCenterChange(const Position&)
{
    requestVisibleTilesCacheUpdate();
}

void MapView::lockFirstVisibleFloor(uint8 firstVisibleFloor)
{
    m_lockedFirstVisibleFloor = firstVisibleFloor;
    requestVisibleTilesCacheUpdate();
}

void MapView::unlockFirstVisibleFloor()
{
    m_lockedFirstVisibleFloor = UINT8_MAX;
    requestVisibleTilesCacheUpdate();
}

void MapView::setVisibleDimension(const Size& visibleDimension)
{
    if(visibleDimension == m_visibleDimension)
        return;

    if(visibleDimension.width() % 2 != 1 || visibleDimension.height() % 2 != 1) {
        g_logger.traceError("visible dimension must be odd");
        return;
    }

    if(visibleDimension < Size(3)) {
        g_logger.traceError("reach max zoom in");
        return;
    }

    updateGeometry(visibleDimension, m_optimizedSize);
}

void MapView::setViewMode(ViewMode viewMode)
{
    m_viewMode = viewMode;
    requestVisibleTilesCacheUpdate();
}

void MapView::setAutoViewMode(bool enable)
{
    m_autoViewMode = enable;
    if(enable)
        updateGeometry(m_visibleDimension, m_optimizedSize);
}

void MapView::optimizeForSize(const Size& visibleSize)
{
    updateGeometry(m_visibleDimension, visibleSize);
}

void MapView::setAntiAliasing(const bool enable)
{
    m_pools.map->setSmooth(enable);

    updateGeometry(m_visibleDimension, m_optimizedSize);
}

void MapView::setRenderScale(const uint8 scale)
{
    m_renderScale = scale;
    updateGeometry(m_visibleDimension, m_optimizedSize);
    updateLight();
}

void MapView::followCreature(const CreaturePtr& creature)
{
    m_follow = true;
    m_followingCreature = creature;
    m_lastCameraPosition = Position();

    requestVisibleTilesCacheUpdate();
}

void MapView::setCameraPosition(const Position& pos)
{
    m_follow = false;
    m_customCameraPosition = pos;
    requestVisibleTilesCacheUpdate();
}

Position MapView::getPosition(const Point& point, const Size& mapSize)
{
    const Position cameraPosition = getCameraPosition();

    // if we have no camera, its impossible to get the tile
    if(!cameraPosition.isValid())
        return Position();

    const Rect srcRect = calcFramebufferSource(mapSize);
    const float sh = srcRect.width() / static_cast<float>(mapSize.width());
    const float sv = srcRect.height() / static_cast<float>(mapSize.height());

    const Point framebufferPos = Point(point.x * sh, point.y * sv);
    const Point centerOffset = (framebufferPos + srcRect.topLeft()) / m_tileSize;

    const Point tilePos2D = getVisibleCenterOffset() - m_drawDimension.toPoint() + centerOffset + Point(2);
    if(tilePos2D.x + cameraPosition.x < 0 && tilePos2D.y + cameraPosition.y < 0)
        return Position();

    Position position = Position(tilePos2D.x, tilePos2D.y, 0) + cameraPosition;

    if(!position.isValid())
        return Position();

    return position;
}

void MapView::move(int32 x, int32 y)
{
    m_moveOffset.x += x;
    m_moveOffset.y += y;

    int32_t tmp = m_moveOffset.x / Otc::TILE_PIXELS;
    bool requestTilesUpdate = false;
    if(tmp != 0) {
        m_customCameraPosition.x += tmp;
        m_moveOffset.x %= Otc::TILE_PIXELS;
        requestTilesUpdate = true;
    }

    tmp = m_moveOffset.y / Otc::TILE_PIXELS;
    if(tmp != 0) {
        m_customCameraPosition.y += tmp;
        m_moveOffset.y %= Otc::TILE_PIXELS;
        requestTilesUpdate = true;
    }

    m_rectCache.rect = Rect();

    if(requestTilesUpdate)
        requestVisibleTilesCacheUpdate();

    onCameraMove(m_moveOffset);
}

Rect MapView::calcFramebufferSource(const Size& destSize)
{
    Point drawOffset = ((m_drawDimension - m_visibleDimension - Size(1)).toPoint() / 2) * m_tileSize;
    if(isFollowingCreature())
        drawOffset += m_followingCreature->getWalkOffset() * m_scaleFactor;
    else if(!m_moveOffset.isNull())
        drawOffset += m_moveOffset * m_scaleFactor;

    Size srcSize = destSize;
    const Size srcVisible = m_visibleDimension * m_tileSize;
    srcSize.scale(srcVisible, Fw::KeepAspectRatio);
    drawOffset.x += (srcVisible.width() - srcSize.width()) / 2;
    drawOffset.y += (srcVisible.height() - srcSize.height()) / 2;

    return Rect(drawOffset, srcSize);
}

uint8 MapView::calcFirstVisibleFloor()
{
    uint8 z = Otc::SEA_FLOOR;
    // return forced first visible floor
    if(m_lockedFirstVisibleFloor != UINT8_MAX) {
        z = m_lockedFirstVisibleFloor;
    } else {
        const Position cameraPosition = getCameraPosition();

        // this could happens if the player is not known yet
        if(cameraPosition.isValid()) {
            // avoid rendering multifloors in far views
            if(!m_multifloor) {
                z = cameraPosition.z;
            } else {
                // if nothing is limiting the view, the first visible floor is 0
                uint8 firstFloor = 0;

                // limits to underground floors while under sea level
                if(cameraPosition.z > Otc::SEA_FLOOR)
                    firstFloor = std::max<uint8>(cameraPosition.z - Otc::AWARE_UNDEGROUND_FLOOR_RANGE, Otc::UNDERGROUND_FLOOR);

                // loop in 3x3 tiles around the camera
                for(int_fast32_t ix = -1; ix <= 1 && firstFloor < cameraPosition.z; ++ix) {
                    for(int_fast32_t iy = -1; iy <= 1 && firstFloor < cameraPosition.z; ++iy) {
                        const Position pos = cameraPosition.translated(ix, iy);

                        // process tiles that we can look through, e.g. windows, doors
                        if((ix == 0 && iy == 0) || ((std::abs(ix) != std::abs(iy)) && g_map.isLookPossible(pos))) {
                            Position upperPos = pos;
                            Position coveredPos = pos;

                            const auto isLookPossible = g_map.isLookPossible(pos);
                            while(coveredPos.coveredUp() && upperPos.up() && upperPos.z >= firstFloor) {
                                // check tiles physically above
                                TilePtr tile = g_map.getTile(upperPos);
                                if(tile && tile->limitsFloorsView(!isLookPossible)) {
                                    firstFloor = upperPos.z + 1;
                                    break;
                                }

                                // check tiles geometrically above
                                tile = g_map.getTile(coveredPos);
                                if(tile && tile->limitsFloorsView(isLookPossible)) {
                                    firstFloor = coveredPos.z + 1;
                                    break;
                                }
                            }
                        }
                    }
                }

                z = firstFloor;
            }
        }
    }

    // just ensure the that the floor is in the valid range
    z = stdext::clamp<int>(z, 0, static_cast<int>(Otc::MAX_Z));
    return z;
}

uint8 MapView::calcLastVisibleFloor()
{
    if(!m_multifloor)
        return calcFirstVisibleFloor();

    uint8 z = Otc::SEA_FLOOR;

    const Position cameraPosition = getCameraPosition();
    // this could happens if the player is not known yet
    if(cameraPosition.isValid()) {
        // view only underground floors when below sea level
        if(cameraPosition.z > Otc::SEA_FLOOR)
            z = cameraPosition.z + Otc::AWARE_UNDEGROUND_FLOOR_RANGE;
        else
            z = Otc::SEA_FLOOR;
    }

    if(m_lockedFirstVisibleFloor != UINT8_MAX)
        z = std::max<int>(m_lockedFirstVisibleFloor, z);

    // just ensure the that the floor is in the valid range
    z = stdext::clamp<int>(z, 0, static_cast<int>(Otc::MAX_Z));
    return z;
}

TilePtr MapView::getTopTile(Position tilePos)
{
    // we must check every floor, from top to bottom to check for a clickable tile
    TilePtr tile;
    tilePos.coveredUp(tilePos.z - m_floorMin);
    for(uint8 i = m_floorMin; i <= m_floorMax; ++i) {
        tile = g_map.getTile(tilePos);
        if(tile && tile->isClickable())
            break;
        tilePos.coveredDown();
    }

    if(!tile || !tile->isClickable())
        return nullptr;

    return tile;
}

Position MapView::getCameraPosition()
{
    if(isFollowingCreature())
        return m_followingCreature->getPosition();

    return m_customCameraPosition;
}

void MapView::setShader(const PainterShaderProgramPtr& shader, float fadein, float fadeout)
{
    if((m_shader == shader))
        return;

    if(fadeout > 0.0f && m_shader) {
        m_nextShader = shader;
        m_shaderSwitchDone = false;
    } else {
        m_shader = shader;
        m_nextShader = nullptr;
        m_shaderSwitchDone = true;
    }
    m_fadeTimer.restart();
    m_fadeInTime = fadein;
    m_fadeOutTime = fadeout;

    if(shader) shader->setPosition(getCameraPosition());
}

void MapView::setDrawLights(bool enable)
{
    if(enable == m_drawLights) return;

    m_lightView = enable ? LightViewPtr(new LightView(this)) : nullptr;
    m_drawLights = enable;

    updateLight();
}

void MapView::updateViewportDirectionCache()
{
    for(uint8 dir = Otc::North; dir <= Otc::InvalidDirection; ++dir) {
        AwareRange& vp = m_viewPortDirection[dir];
        vp.top = m_awareRange.top;
        vp.right = m_awareRange.right;
        vp.bottom = vp.top;
        vp.left = vp.right;

        switch(dir) {
        case Otc::North:
        case Otc::South:
            vp.top += 1;
            vp.bottom += 1;
            break;

        case Otc::West:
        case Otc::East:
            vp.right += 1;
            vp.left += 1;
            break;

        case Otc::NorthEast:
        case Otc::SouthEast:
        case Otc::NorthWest:
        case Otc::SouthWest:
            vp.left += 1;
            vp.bottom += 1;
            vp.top += 1;
            vp.right += 1;
            break;

        case Otc::InvalidDirection:
            vp.left -= 1;
            vp.right -= 1;
            break;

        default:
            break;
        }
    }
}

std::vector<CreaturePtr> MapView::getSightSpectators(const Position& centerPos, bool multiFloor)
{
    return g_map.getSpectatorsInRangeEx(centerPos, multiFloor, m_awareRange.left - 1, m_awareRange.right - 2, m_awareRange.top - 1, m_awareRange.bottom - 2);
}

std::vector<CreaturePtr> MapView::getSpectators(const Position& centerPos, bool multiFloor)
{
    return g_map.getSpectatorsInRangeEx(centerPos, multiFloor, m_awareRange.left, m_awareRange.right, m_awareRange.top, m_awareRange.bottom);
}

bool MapView::isInRange(const Position& pos, const bool ignoreZ)
{
    return getCameraPosition().isInRange(pos, m_awareRange.left - 1, m_awareRange.right - 2, m_awareRange.top - 1, m_awareRange.bottom - 2, ignoreZ);
}

void MapView::setCrosshairTexture(const std::string& texturePath)
{
    m_crosshairTexture = texturePath.empty() ? nullptr : g_textures.getTexture(texturePath);
}
