import { useState, ReactNode } from 'react';

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
  const [activeTab, setActiveTab] = useState<TabId>(defaultTab);

  const activeContent = tabs.find(tab => tab.id === activeTab)?.content;

  return (
    <div className="tab-view">
      <div className="tab-header">
        {tabs.map(tab => (
          <button
            key={tab.id}
            className={`tab-button ${activeTab === tab.id ? 'active' : ''}`}
            onClick={() => setActiveTab(tab.id)}
          >
            {tab.label}
          </button>
        ))}
      </div>
      <div className="tab-content">{activeContent}</div>
    </div>
  );
}
