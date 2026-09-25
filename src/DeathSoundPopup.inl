class DeathSoundPopup : public geode::Popup {
protected:
    WeakRef<SectionListPopup> m_parent;
    int m_index = -1;
    SectionData m_section;
    bool m_customTab = false;
    bool m_deleteMode = false;
    CCNode* m_listContent = nullptr;
    CCLabelBMFont* m_geometryTabLabel = nullptr;
    CCLabelBMFont* m_customTabLabel = nullptr;
    CCLabelBMFont* m_deleteModeLabel = nullptr;
    CCLabelBMFont* m_volumeLabel = nullptr;
    Slider* m_volumeSlider = nullptr;
    std::vector<DeathSoundOption> m_geometrySounds;

    void notifyParent() {
        if (auto parent = m_parent.lock()) {
            parent->setDeathSound(m_index, m_section);
        }
    }

    CCNode* createSoundRow(std::string const& label, bool selected) {
        auto holder = CCNode::create();
        holder->setContentSize({285.f, 26.f});
        holder->setAnchorPoint({0.5f, 0.5f});
        holder->ignoreAnchorPointForPosition(false);

        auto background = CCLayerColor::create(
            selected ? ccc4(35, 88, 48, 150) : ccc4(20, 28, 23, 105),
            285.f,
            24.f
        );
        background->setPosition({0.f, 1.f});
        holder->addChild(background);

        auto marker = CCSprite::createWithSpriteFrameName(
            selected ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png"
        );
        if (marker) {
            marker->setScale(0.42f);
            marker->setPosition({14.f, 13.f});
            holder->addChild(marker);
        }

        auto name = CCLabelBMFont::create(label.c_str(), "bigFont.fnt");
        name->setAnchorPoint({0.f, 0.5f});
        name->setScale(0.29f);
        name->limitLabelWidth(245.f, 0.29f, 0.17f);
        name->setColor(
            selected ? ccc3(115, 255, 145) : ccc3(235, 245, 238)
        );
        name->setPosition({28.f, 13.f});
        holder->addChild(name);
        return holder;
    }

    CCNode* createPreviewIcon() {
        auto holder = CCNode::create();
        holder->setContentSize({24.f, 24.f});
        holder->setAnchorPoint({0.5f, 0.5f});
        holder->ignoreAnchorPointForPosition(false);

        auto drawing = CCDrawNode::create();
        auto const color = ccc4f(0.25f, 1.f, 0.42f, 1.f);
        drawing->drawSegment({8.f, 6.f}, {8.f, 18.f}, 1.2f, color);
        drawing->drawSegment({8.f, 18.f}, {18.f, 12.f}, 1.2f, color);
        drawing->drawSegment({18.f, 12.f}, {8.f, 6.f}, 1.2f, color);
        holder->addChild(drawing);
        return holder;
    }

    void updateTabLabels() {
        if (m_geometryTabLabel) {
            m_geometryTabLabel->setColor(
                m_customTab ? ccc3(180, 180, 180) : ccc3(80, 255, 120)
            );
        }
        if (m_customTabLabel) {
            m_customTabLabel->setColor(
                m_customTab ? ccc3(80, 255, 120) : ccc3(180, 180, 180)
            );
        }
    }

    void reloadList() {
        if (m_listContent) m_listContent->removeFromParent();
        m_listContent = CCNode::create();
        m_mainLayer->addChild(m_listContent);
        updateTabLabels();

        auto const customSounds = loadCustomDeathSounds();
        auto const rowCount = m_customTab
            ? customSounds.size()
            : m_geometrySounds.size();
        auto const viewportWidth = m_customTab ? 322.f : 390.f;
        auto const contentHeight = std::max(
            145.f,
            static_cast<float>(rowCount) * 29.f + 4.f
        );

        auto scroll = ScrollLayer::create({viewportWidth, 145.f});
        scroll->setStealingTouches(true);
        scroll->setPosition({15.f, 60.f});
        scroll->m_contentLayer->setContentSize({viewportWidth, contentHeight});
        m_listContent->addChild(scroll);

        auto rowMenu = CCMenu::create();
        rowMenu->setPosition({0.f, 0.f});
        scroll->m_contentLayer->addChild(rowMenu);

        auto previewMenu = CCMenu::create();
        previewMenu->setPosition({0.f, 0.f});
        scroll->m_contentLayer->addChild(previewMenu);

        for (int index = 0; index < static_cast<int>(rowCount); ++index) {
            std::string label;
            std::string path;
            if (m_customTab) {
                path = customSounds[index];
                label = customDeathSoundName(path);
            }
            else {
                auto const& option = m_geometrySounds[index];
                path = option.path;
                label = option.label;
            }

            auto const selected =
                m_section.deathSoundCustom == m_customTab &&
                m_section.deathSound == path;
            auto const y = contentHeight - 16.f - index * 29.f;

            auto rowButton = CCMenuItemSpriteExtra::create(
                createSoundRow(label, selected),
                this,
                menu_selector(DeathSoundPopup::onSelectSound)
            );
            rowButton->setTag(index);
            rowButton->setPosition({151.f, y});
            rowMenu->addChild(rowButton);

            auto previewButton = CCMenuItemSpriteExtra::create(
                createPreviewIcon(),
                this,
                menu_selector(DeathSoundPopup::onPreviewSound)
            );
            previewButton->setTag(index);
            previewButton->setPosition({
                m_customTab ? 309.f : 365.f,
                y
            });
            previewMenu->addChild(previewButton);
        }

        if (rowCount == 0) {
            auto emptyLabel = CCLabelBMFont::create(
                "No custom sounds. Press Add.",
                "bigFont.fnt"
            );
            emptyLabel->setScale(0.32f);
            emptyLabel->setColor(ccc3(170, 195, 178));
            emptyLabel->setPosition({viewportWidth / 2.f, 72.f});
            scroll->m_contentLayer->addChild(emptyLabel);
        }
        scroll->moveToTop();

        if (!m_customTab) return;

        auto toolMenu = CCMenu::create();
        toolMenu->setPosition({0.f, 0.f});
        m_listContent->addChild(toolMenu);

        auto addSprite = ButtonSprite::create("Add");
        addSprite->setScale(0.48f);
        auto addButton = CCMenuItemSpriteExtra::create(
            addSprite,
            this,
            menu_selector(DeathSoundPopup::onAddCustomSound)
        );
        addButton->setPosition({378.f, 170.f});
        toolMenu->addChild(addButton);

        auto deleteToggle = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(DeathSoundPopup::onToggleDeleteMode),
            0.58f
        );
        deleteToggle->toggle(m_deleteMode);
        deleteToggle->setPosition({378.f, 116.f});
        toolMenu->addChild(deleteToggle);

        m_deleteModeLabel = CCLabelBMFont::create(
            "Delete\nMode",
            "bigFont.fnt"
        );
        m_deleteModeLabel->setAlignment(kCCTextAlignmentCenter);
        m_deleteModeLabel->setScale(0.22f);
        m_deleteModeLabel->setColor(
            m_deleteMode ? ccc3(255, 90, 90) : ccc3(210, 220, 212)
        );
        m_deleteModeLabel->setPosition({378.f, 83.f});
        m_listContent->addChild(m_deleteModeLabel);
    }

    bool init(
        SectionListPopup* parent,
        int index,
        SectionData section
    ) {
        if (!Popup::init(420.f, 290.f)) return false;
        m_parent = parent;
        m_index = index;
        m_section = std::move(section);
        m_geometrySounds = geometryDashDeathSounds();
        m_section.deathSoundVolume = std::clamp(
            m_section.deathSoundVolume,
            0.f,
            1.f
        );
        if (m_section.deathSound.empty()) {
            m_section.deathSound = "explode_11.ogg";
            m_section.deathSoundCustom = false;
        }
        m_customTab = m_section.deathSoundCustom;
        this->setID("death-sound-popup"_spr);
        this->setTitle("Death Sound");

        auto overrideLabel = CCLabelBMFont::create(
            "Death Sound Override",
            "bigFont.fnt"
        );
        overrideLabel->setAnchorPoint({0.f, 0.5f});
        overrideLabel->setScale(0.32f);
        overrideLabel->setPosition({22.f, 230.f});
        m_mainLayer->addChild(overrideLabel);

        auto overrideToggle = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(DeathSoundPopup::onToggleOverride),
            0.58f
        );
        overrideToggle->toggle(m_section.deathSoundOverride);
        overrideToggle->setPosition({160.f, 230.f});
        m_buttonMenu->addChild(overrideToggle);

        auto geometrySprite = ButtonSprite::create("Geometry Dash");
        geometrySprite->setScale(0.4f);
        auto geometryButton = CCMenuItemSpriteExtra::create(
            geometrySprite,
            this,
            menu_selector(DeathSoundPopup::onShowGeometryDash)
        );
        geometryButton->setPosition({262.f, 230.f});
        m_buttonMenu->addChild(geometryButton);

        auto customSprite = ButtonSprite::create("Custom");
        customSprite->setScale(0.45f);
        auto customButton = CCMenuItemSpriteExtra::create(
            customSprite,
            this,
            menu_selector(DeathSoundPopup::onShowCustom)
        );
        customButton->setPosition({365.f, 230.f});
        m_buttonMenu->addChild(customButton);

        m_geometryTabLabel = CCLabelBMFont::create(
            "Geometry Dash",
            "bigFont.fnt"
        );
        m_geometryTabLabel->setScale(0.24f);
        m_geometryTabLabel->setPosition({262.f, 253.f});
        m_mainLayer->addChild(m_geometryTabLabel);

        m_customTabLabel = CCLabelBMFont::create("Custom", "bigFont.fnt");
        m_customTabLabel->setScale(0.24f);
        m_customTabLabel->setPosition({365.f, 253.f});
        m_mainLayer->addChild(m_customTabLabel);

        auto listPanel = CCScale9Sprite::create("square02_001.png");
        if (listPanel) {
            listPanel->setContentSize({396.f, 151.f});
            listPanel->setColor(ccc3(28, 48, 34));
            listPanel->setOpacity(205);
            listPanel->setPosition({210.f, 132.5f});
            m_mainLayer->addChild(listPanel);
        }

        auto volumeCaption = CCLabelBMFont::create("Volume", "bigFont.fnt");
        volumeCaption->setScale(0.3f);
        volumeCaption->setAnchorPoint({0.f, 0.5f});
        volumeCaption->setPosition({20.f, 31.f});
        m_mainLayer->addChild(volumeCaption);

        m_volumeSlider = Slider::create(
            this,
            menu_selector(DeathSoundPopup::onVolumeChanged),
            0.85f
        );
        if (m_volumeSlider) {
            m_volumeSlider->setPosition({235.f, 31.f});
            m_volumeSlider->setValue(m_section.deathSoundVolume);
            m_volumeSlider->setLiveDragging(true);
            m_mainLayer->addChild(m_volumeSlider);
        }

        m_volumeLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_volumeLabel->setScale(0.35f);
        m_volumeLabel->setAnchorPoint({1.f, 0.5f});
        m_volumeLabel->setPosition({402.f, 31.f});
        m_mainLayer->addChild(m_volumeLabel);
        updateVolumeLabel();

        reloadList();
        return true;
    }

    void updateVolumeLabel() {
        if (m_volumeLabel) {
            m_volumeLabel->setString(
                fmt::format(
                    "{:.0f}%",
                    m_section.deathSoundVolume * 100.f
                ).c_str()
            );
        }
    }

    std::string soundPathAt(int index) const {
        if (m_customTab) {
            auto sounds = loadCustomDeathSounds();
            return index >= 0 && index < static_cast<int>(sounds.size())
                ? sounds[index]
                : std::string();
        }
        auto const& options = m_geometrySounds;
        return index >= 0 && index < static_cast<int>(options.size())
            ? options[index].path
            : std::string();
    }

    void onToggleOverride(CCObject*) {
        m_section.deathSoundOverride = !m_section.deathSoundOverride;
        notifyParent();
    }

    void onShowGeometryDash(CCObject*) {
        if (!m_customTab) return;
        m_customTab = false;
        reloadList();
    }

    void onShowCustom(CCObject*) {
        if (m_customTab) return;
        m_customTab = true;
        reloadList();
    }

    void onToggleDeleteMode(CCObject*) {
        m_deleteMode = !m_deleteMode;
        if (m_deleteModeLabel) {
            m_deleteModeLabel->setColor(
                m_deleteMode ? ccc3(255, 90, 90) : ccc3(210, 220, 212)
            );
        }
    }

    void onSelectSound(CCObject* sender) {
        auto const index = static_cast<CCNode*>(sender)->getTag();
        auto path = soundPathAt(index);
        if (path.empty()) return;

        if (m_customTab && m_deleteMode) {
            std::error_code error;
            std::filesystem::remove(path, error);
            if (error) {
                FLAlertLayer::create(
                    "Delete Failed",
                    fmt::format(
                        "Could not delete sound: {}",
                        error.message()
                    ),
                    "OK"
                )->show();
                return;
            }

            auto sounds = loadCustomDeathSounds();
            sounds.erase(
                std::remove(sounds.begin(), sounds.end(), path),
                sounds.end()
            );
            saveCustomDeathSounds(sounds);
            if (auto parent = m_parent.lock()) {
                parent->removeCustomDeathSoundReferences(path);
            }
            if (m_section.deathSoundCustom && m_section.deathSound == path) {
                m_section.deathSoundOverride = false;
                m_section.deathSound = "explode_11.ogg";
                m_section.deathSoundCustom = false;
            }
            reloadList();
            return;
        }

        m_section.deathSound = std::move(path);
        m_section.deathSoundCustom = m_customTab;
        if (!m_customTab && !ensureDeathSoundAvailable(m_section.deathSound)) {
            Notification::create(
                "Downloading SFX...",
                NotificationIcon::Info
            )->show();
        }
        notifyParent();
        reloadList();
    }

    void onPreviewSound(CCObject* sender) {
        auto const index = static_cast<CCNode*>(sender)->getTag();
        auto path = soundPathAt(index);
        if (!path.empty()) {
            previewDeathSound(path, m_section.deathSoundVolume);
        }
    }

    void onVolumeChanged(CCObject* sender) {
        auto thumb = static_cast<SliderThumb*>(sender);
        if (!thumb) return;
        m_section.deathSoundVolume = std::clamp(
            thumb->getValue(),
            0.f,
            1.f
        );
        updateVolumeLabel();
        notifyParent();
    }

    void onAddCustomSound(CCObject*) {
        WeakRef<DeathSoundPopup> self(this);
        async::spawn(file::pick(file::PickMode::OpenFile, {
            .filters = {{
                .description = "Audio",
                .files = {"*.mp3", "*.wav"}
            }}
        }), [self](Result<std::optional<std::filesystem::path>> result) {
            auto popup = self.lock();
            if (!popup) return;
            if (result.isErr()) {
                FLAlertLayer::create(
                    "Import Failed",
                    result.unwrapErr(),
                    "OK"
                )->show();
                return;
            }
            if (!result.unwrap().has_value()) return;

            auto source = result.unwrap().value();
            auto extension = source.extension().string();
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                }
            );
            if (extension != ".mp3" && extension != ".wav") {
                FLAlertLayer::create(
                    "Import Failed",
                    "Only MP3 and WAV files are supported.",
                    "OK"
                )->show();
                return;
            }

            auto directory = Mod::get()->getSaveDir() / "death-sounds";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            auto destination = directory / source.filename();
            if (
                std::filesystem::exists(destination) ||
                std::filesystem::equivalent(source, destination, error)
            ) {
                error.clear();
                auto stamp = std::chrono::steady_clock::now()
                    .time_since_epoch().count();
                destination = directory / fmt::format(
                    "{}-{}{}",
                    source.stem().string(),
                    stamp,
                    extension
                );
            }
            error.clear();
            std::filesystem::copy_file(
                source,
                destination,
                std::filesystem::copy_options::overwrite_existing,
                error
            );
            if (error) {
                FLAlertLayer::create(
                    "Import Failed",
                    fmt::format(
                        "Could not copy sound: {}",
                        error.message()
                    ),
                    "OK"
                )->show();
                return;
            }

            auto sounds = loadCustomDeathSounds();
            auto path = destination.string();
            sounds.push_back(path);
            saveCustomDeathSounds(sounds);
            popup->m_section.deathSound = path;
            popup->m_section.deathSoundCustom = true;
            popup->notifyParent();
            popup->reloadList();
        });
    }

public:
    void onClose(CCObject* sender) override {
        if (auto parent = m_parent.lock()) {
            parent->refreshSectionRows();
            parent->setSectionControlsEnabled(true);
        }
        Popup::onClose(sender);
    }

    static DeathSoundPopup* create(
        SectionListPopup* parent,
        int index,
        SectionData section
    ) {
        auto ret = new DeathSoundPopup();
        if (ret && ret->init(parent, index, std::move(section))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void SectionListPopup::onOpenDeathSound(CCObject* sender) {
    auto const index = static_cast<CCNode*>(sender)->getTag();
    if (index < 0 || index >= static_cast<int>(m_sections.size())) {
        return;
    }
    if (auto popup = DeathSoundPopup::create(this, index, m_sections[index])) {
        setSectionControlsEnabled(false);
        popup->show();
    }
}
