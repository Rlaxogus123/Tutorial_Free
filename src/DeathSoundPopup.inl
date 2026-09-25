class DeathSoundPopup : public geode::Popup {
protected:
    static constexpr int SOUNDS_PER_PAGE = 5;

    WeakRef<SectionListPopup> m_parent;
    DeathSoundSettings m_settings;
    bool m_customTab = false;
    bool m_deleteMode = false;
    int m_page = 0;
    CCNode* m_listContent = nullptr;
    CCLabelBMFont* m_geometryTabLabel = nullptr;
    CCLabelBMFont* m_customTabLabel = nullptr;
    CCLabelBMFont* m_deleteModeLabel = nullptr;
    CCLabelBMFont* m_volumeLabel = nullptr;
    Slider* m_volumeSlider = nullptr;
    std::vector<DeathSoundOption> m_geometrySounds;

    void notifyParent() {
        if (auto parent = m_parent.lock()) {
            parent->setDeathSoundSettings(m_settings);
        }
    }

    CCNode* createSoundRow(std::string const& label, bool selected) {
        auto holder = CCNode::create();
        holder->setContentSize({365.f, 24.f});
        holder->setAnchorPoint({0.5f, 0.5f});
        holder->ignoreAnchorPointForPosition(false);

        auto background = CCLayerColor::create(
            selected ? ccc4(35, 88, 48, 180) : ccc4(12, 22, 16, 150),
            365.f,
            23.f
        );
        background->setPosition({0.f, 0.5f});
        holder->addChild(background);

        auto marker = CCSprite::createWithSpriteFrameName(
            selected ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png"
        );
        if (marker) {
            marker->setScale(0.38f);
            marker->setPosition({13.f, 12.f});
            holder->addChild(marker);
        }

        auto name = CCLabelBMFont::create(label.c_str(), "bigFont.fnt");
        name->setAnchorPoint({0.f, 0.5f});
        name->setScale(0.28f);
        name->limitLabelWidth(325.f, 0.28f, 0.16f);
        name->setColor(
            selected ? ccc3(100, 255, 130) : ccc3(235, 245, 238)
        );
        name->setPosition({27.f, 12.f});
        holder->addChild(name);
        return holder;
    }

    CCNode* createPreviewIcon() {
        auto holder = CCNode::create();
        holder->setContentSize({24.f, 24.f});
        holder->setAnchorPoint({0.5f, 0.5f});
        holder->ignoreAnchorPointForPosition(false);

        auto drawing = CCDrawNode::create();
        auto const green = ccc4f(0.f, 1.f, 0.f, 1.f);
        CCPoint triangle[] = {{7.f, 5.f}, {19.f, 12.f}, {7.f, 19.f}};
        drawing->drawPolygon(triangle, 3, green, 0.f, green);
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

    std::size_t currentSoundCount() const {
        return m_customTab
            ? loadCustomDeathSounds().size()
            : m_geometrySounds.size();
    }

    int maxPage() const {
        auto const count = currentSoundCount();
        return count == 0
            ? 0
            : static_cast<int>((count - 1) / SOUNDS_PER_PAGE);
    }

    std::string soundPathAt(int index) const {
        if (m_customTab) {
            auto sounds = loadCustomDeathSounds();
            return index >= 0 && index < static_cast<int>(sounds.size())
                ? sounds[index]
                : std::string();
        }
        return index >= 0 && index < static_cast<int>(m_geometrySounds.size())
            ? m_geometrySounds[index].path
            : std::string();
    }

    void reloadList() {
        if (m_listContent) m_listContent->removeFromParent();
        m_listContent = CCNode::create();
        m_mainLayer->addChild(m_listContent, 3);
        updateTabLabels();

        auto const customSounds = loadCustomDeathSounds();
        auto const rowCount = m_customTab
            ? static_cast<int>(customSounds.size())
            : static_cast<int>(m_geometrySounds.size());
        m_page = std::clamp(m_page, 0, maxPage());
        auto const first = m_page * SOUNDS_PER_PAGE;
        auto const last = std::min(first + SOUNDS_PER_PAGE, rowCount);

        auto rowMenu = CCMenu::create();
        rowMenu->setPosition({0.f, 0.f});
        m_listContent->addChild(rowMenu);

        auto previewMenu = CCMenu::create();
        previewMenu->setPosition({0.f, 0.f});
        m_listContent->addChild(previewMenu);

        for (int index = first; index < last; ++index) {
            std::string label;
            std::string path;
            if (m_customTab) {
                path = customSounds[index];
                label = customDeathSoundName(path);
            }
            else {
                path = m_geometrySounds[index].path;
                label = m_geometrySounds[index].label;
            }

            auto const selected =
                m_settings.custom == m_customTab &&
                m_settings.sound == path;
            auto const row = index - first;
            auto const y = 196.f - row * 27.f;

            auto rowButton = CCMenuItemSpriteExtra::create(
                createSoundRow(label, selected),
                this,
                menu_selector(DeathSoundPopup::onSelectSound)
            );
            rowButton->setTag(index);
            rowButton->setPosition({199.f, y});
            rowMenu->addChild(rowButton);

            auto previewButton = CCMenuItemSpriteExtra::create(
                createPreviewIcon(),
                this,
                menu_selector(DeathSoundPopup::onPreviewSound)
            );
            previewButton->setTag(index);
            previewButton->setPosition({426.f, y});
            previewMenu->addChild(previewButton);
        }

        if (rowCount == 0) {
            auto emptyLabel = CCLabelBMFont::create(
                m_customTab
                    ? "No custom sounds. Press Add."
                    : "No packaged Geometry Dash sounds found.",
                "bigFont.fnt"
            );
            emptyLabel->setScale(0.3f);
            emptyLabel->setColor(ccc3(170, 195, 178));
            emptyLabel->setPosition({230.f, 140.f});
            m_listContent->addChild(emptyLabel);
        }

        auto navigation = CCMenu::create();
        navigation->setPosition({0.f, 0.f});
        m_listContent->addChild(navigation);

        auto leftSprite = CCSprite::createWithSpriteFrameName(
            "GJ_arrow_03_001.png"
        );
        auto rightSprite = CCSprite::createWithSpriteFrameName(
            "GJ_arrow_03_001.png"
        );
        if (leftSprite && rightSprite) {
            leftSprite->setScale(0.32f);
            rightSprite->setScale(0.32f);
            rightSprite->setFlipX(true);
            auto left = CCMenuItemSpriteExtra::create(
                leftSprite,
                this,
                menu_selector(DeathSoundPopup::onChangePage)
            );
            auto right = CCMenuItemSpriteExtra::create(
                rightSprite,
                this,
                menu_selector(DeathSoundPopup::onChangePage)
            );
            left->setTag(-1);
            right->setTag(1);
            left->setPosition({200.f, 67.f});
            right->setPosition({260.f, 67.f});
            left->setEnabled(m_page > 0);
            right->setEnabled(m_page < maxPage());
            navigation->addChild(left);
            navigation->addChild(right);
        }

        auto pageLabel = CCLabelBMFont::create(
            fmt::format("{} / {}", m_page + 1, maxPage() + 1).c_str(),
            "goldFont.fnt"
        );
        pageLabel->setScale(0.28f);
        pageLabel->setPosition({230.f, 67.f});
        m_listContent->addChild(pageLabel);

        if (!m_customTab) return;

        auto toolMenu = CCMenu::create();
        toolMenu->setPosition({0.f, 0.f});
        m_listContent->addChild(toolMenu);

        auto addSprite = ButtonSprite::create("Add");
        addSprite->setScale(0.43f);
        auto addButton = CCMenuItemSpriteExtra::create(
            addSprite,
            this,
            menu_selector(DeathSoundPopup::onAddCustomSound)
        );
        addButton->setPosition({385.f, 31.f});
        toolMenu->addChild(addButton);

        auto deleteToggle = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(DeathSoundPopup::onToggleDeleteMode),
            0.48f
        );
        deleteToggle->toggle(m_deleteMode);
        deleteToggle->setPosition({438.f, 32.f});
        toolMenu->addChild(deleteToggle);

        m_deleteModeLabel = CCLabelBMFont::create("DEL", "bigFont.fnt");
        m_deleteModeLabel->setScale(0.17f);
        m_deleteModeLabel->setColor(
            m_deleteMode ? ccc3(255, 90, 90) : ccc3(210, 220, 212)
        );
        m_deleteModeLabel->setPosition({438.f, 14.f});
        m_listContent->addChild(m_deleteModeLabel);
    }

    bool init(SectionListPopup* parent) {
        if (!Popup::init(460.f, 310.f)) return false;
        m_parent = parent;
        m_settings = loadDeathSoundSettings();
        m_geometrySounds = geometryDashDeathSounds();
        m_settings.minimumPercent = std::clamp(
            m_settings.minimumPercent,
            0.f,
            100.f
        );
        m_settings.volume = std::clamp(m_settings.volume, 0.f, 1.f);
        if (m_settings.sound.empty()) {
            m_settings.sound = "explode_11.ogg";
            m_settings.custom = false;
        }
        m_customTab = m_settings.custom;
        this->setID("death-sound-popup"_spr);
        this->setTitle("Death Sound");

        auto enableLabel = CCLabelBMFont::create("Enable", "bigFont.fnt");
        enableLabel->setAnchorPoint({0.f, 0.5f});
        enableLabel->setScale(0.3f);
        enableLabel->setPosition({18.f, 246.f});
        m_mainLayer->addChild(enableLabel);

        auto enableToggle = CCMenuItemToggler::createWithStandardSprites(
            this,
            menu_selector(DeathSoundPopup::onToggleOverride),
            0.52f
        );
        enableToggle->toggle(m_settings.enabled);
        enableToggle->setPosition({83.f, 246.f});
        m_buttonMenu->addChild(enableToggle);

        auto thresholdLabel = CCLabelBMFont::create("From", "bigFont.fnt");
        thresholdLabel->setAnchorPoint({0.f, 0.5f});
        thresholdLabel->setScale(0.28f);
        thresholdLabel->setPosition({111.f, 246.f});
        m_mainLayer->addChild(thresholdLabel);

        auto thresholdInput = TextInput::create(58.f, "0");
        thresholdInput->setString(fmt::format(
            "{:.1f}",
            m_settings.minimumPercent
        ));
        thresholdInput->setScale(0.62f);
        thresholdInput->setPosition({176.f, 246.f});
        thresholdInput->setCommonFilter(CommonFilter::Float);
        thresholdInput->setCallback([this](std::string const& value) {
            if (value.empty()) return;
            try {
                m_settings.minimumPercent = std::clamp(
                    std::stof(value),
                    0.f,
                    100.f
                );
                notifyParent();
            }
            catch (...) {}
        });
        m_mainLayer->addChild(thresholdInput);

        auto percentLabel = CCLabelBMFont::create("%+", "bigFont.fnt");
        percentLabel->setAnchorPoint({0.f, 0.5f});
        percentLabel->setScale(0.25f);
        percentLabel->setPosition({202.f, 246.f});
        m_mainLayer->addChild(percentLabel);

        auto geometrySprite = ButtonSprite::create("Geometry Dash");
        geometrySprite->setScale(0.37f);
        auto geometryButton = CCMenuItemSpriteExtra::create(
            geometrySprite,
            this,
            menu_selector(DeathSoundPopup::onShowGeometryDash)
        );
        geometryButton->setPosition({323.f, 239.f});
        m_buttonMenu->addChild(geometryButton);

        auto customSprite = ButtonSprite::create("Custom");
        customSprite->setScale(0.42f);
        auto customButton = CCMenuItemSpriteExtra::create(
            customSprite,
            this,
            menu_selector(DeathSoundPopup::onShowCustom)
        );
        customButton->setPosition({420.f, 239.f});
        m_buttonMenu->addChild(customButton);

        m_geometryTabLabel = CCLabelBMFont::create(
            "Geometry Dash",
            "bigFont.fnt"
        );
        m_geometryTabLabel->setScale(0.22f);
        m_geometryTabLabel->setPosition({323.f, 262.f});
        m_mainLayer->addChild(m_geometryTabLabel);

        m_customTabLabel = CCLabelBMFont::create("Custom", "bigFont.fnt");
        m_customTabLabel->setScale(0.22f);
        m_customTabLabel->setPosition({420.f, 262.f});
        m_mainLayer->addChild(m_customTabLabel);

        auto listPanel = CCScale9Sprite::create("square02_001.png");
        if (listPanel) {
            listPanel->setContentSize({436.f, 151.f});
            listPanel->setColor(ccc3(20, 38, 26));
            listPanel->setOpacity(220);
            listPanel->setPosition({230.f, 137.f});
            m_mainLayer->addChild(listPanel, 1);
        }

        auto volumeCaption = CCLabelBMFont::create("Volume", "bigFont.fnt");
        volumeCaption->setScale(0.27f);
        volumeCaption->setAnchorPoint({0.f, 0.5f});
        volumeCaption->setPosition({15.f, 31.f});
        m_mainLayer->addChild(volumeCaption);

        m_volumeSlider = Slider::create(
            this,
            menu_selector(DeathSoundPopup::onVolumeChanged),
            0.72f
        );
        if (m_volumeSlider) {
            m_volumeSlider->setPosition({205.f, 31.f});
            m_volumeSlider->setValue(m_settings.volume);
            m_volumeSlider->setLiveDragging(true);
            m_mainLayer->addChild(m_volumeSlider);
        }

        m_volumeLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_volumeLabel->setScale(0.3f);
        m_volumeLabel->setAnchorPoint({1.f, 0.5f});
        m_volumeLabel->setPosition({335.f, 31.f});
        m_mainLayer->addChild(m_volumeLabel);
        updateVolumeLabel();

        log::info(
            "Death sound popup loaded {} packaged GD sounds",
            m_geometrySounds.size()
        );
        reloadList();
        return true;
    }

    void updateVolumeLabel() {
        if (m_volumeLabel) {
            m_volumeLabel->setString(fmt::format(
                "{:.0f}%",
                m_settings.volume * 100.f
            ).c_str());
        }
    }

    void onToggleOverride(CCObject*) {
        m_settings.enabled = !m_settings.enabled;
        notifyParent();
    }

    void onShowGeometryDash(CCObject*) {
        if (!m_customTab) return;
        m_customTab = false;
        m_page = 0;
        reloadList();
    }

    void onShowCustom(CCObject*) {
        if (m_customTab) return;
        m_customTab = true;
        m_page = 0;
        reloadList();
    }

    void onChangePage(CCObject* sender) {
        m_page = std::clamp(
            m_page + static_cast<CCNode*>(sender)->getTag(),
            0,
            maxPage()
        );
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
                    fmt::format("Could not delete sound: {}", error.message()),
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
            if (m_settings.custom && m_settings.sound == path) {
                m_settings.enabled = false;
                m_settings.sound = "explode_11.ogg";
                m_settings.custom = false;
                notifyParent();
            }
            reloadList();
            return;
        }

        m_settings.sound = std::move(path);
        m_settings.custom = m_customTab;
        notifyParent();
        reloadList();
    }

    void onPreviewSound(CCObject* sender) {
        auto const index = static_cast<CCNode*>(sender)->getTag();
        auto path = soundPathAt(index);
        if (!path.empty()) previewDeathSound(path, m_settings.volume);
    }

    void onVolumeChanged(CCObject* sender) {
        auto thumb = static_cast<SliderThumb*>(sender);
        if (!thumb) return;
        m_settings.volume = std::clamp(thumb->getValue(), 0.f, 1.f);
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
            if (std::filesystem::exists(destination)) {
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
                    fmt::format("Could not copy sound: {}", error.message()),
                    "OK"
                )->show();
                return;
            }

            auto sounds = loadCustomDeathSounds();
            auto path = destination.string();
            sounds.push_back(path);
            saveCustomDeathSounds(sounds);
            popup->m_settings.sound = path;
            popup->m_settings.custom = true;
            popup->m_page = static_cast<int>(
                (sounds.size() - 1) / SOUNDS_PER_PAGE
            );
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

    static DeathSoundPopup* create(SectionListPopup* parent) {
        auto ret = new DeathSoundPopup();
        if (ret && ret->init(parent)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void SectionListPopup::onOpenDeathSound(CCObject*) {
    if (auto popup = DeathSoundPopup::create(this)) {
        setSectionControlsEnabled(false);
        popup->show();
    }
}
