import { useState, ReactNode, useCallback, KeyboardEvent } from 'react';

export type TabId = 'inputs' | 'generators' | 'effects';

interface Tab {
  id: TabId;
  label: string;
  content: ReactNode;
}

interface TabViewProps {
  tabs: Tab[];
  defaultTab?: TabId;
}

export function TabView({ tabs, defaultTab = 'inputs' }: TabViewProps) {
  // Validate defaultTab exists in tabs array, fallback to first tab if not
  const initialActiveTab: TabId = tabs.some(tab => tab.id === defaultTab)
    ? defaultTab
    : (tabs[0]?.id ?? defaultTab);

  const [activeTab, setActiveTab] = useState<TabId>(initialActiveTab);

  const activeContent = tabs.find(tab => tab.id === activeTab)?.content;

  const handleKeyDown = useCallback(
    (event: KeyboardEvent<HTMLButtonElement>, index: number) => {
      let newIndex = index;

      switch (event.key) {
        case 'ArrowLeft':
          event.preventDefault();
          newIndex = index > 0 ? index - 1 : tabs.length - 1;
          break;
        case 'ArrowRight':
          event.preventDefault();
          newIndex = index < tabs.length - 1 ? index + 1 : 0;
          break;
        case 'Home':
          event.preventDefault();
          newIndex = 0;
          break;
        case 'End':
          event.preventDefault();
          newIndex = tabs.length - 1;
          break;
        default:
          return;
      }

      setActiveTab(tabs[newIndex].id);
    },
    [tabs]
  );

  return (
    <div className="tab-view">
      <div className="tab-header" role="tablist" aria-label="Settings categories">
        {tabs.map((tab, index) => (
          <button
            key={tab.id}
            className={`tab-button ${activeTab === tab.id ? 'active' : ''}`}
            role="tab"
            aria-selected={activeTab === tab.id}
            aria-controls={`tabpanel-${tab.id}`}
            id={`tab-${tab.id}`}
            tabIndex={activeTab === tab.id ? 0 : -1}
            onClick={() => setActiveTab(tab.id)}
            onKeyDown={e => handleKeyDown(e, index)}
          >
            {tab.label}
          </button>
        ))}
      </div>
      <div
        className="tab-content"
        role="tabpanel"
        id={`tabpanel-${activeTab}`}
        aria-labelledby={`tab-${activeTab}`}
        tabIndex={0}
      >
        {activeContent}
      </div>
    </div>
  );
}
